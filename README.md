# twitninf-autorenew

Renouvellement automatique des abonnements TwitNinf, prélevé sur les
portefeuilles NF. Démon C, une seule dépendance : `libpq`.

## Le principe

Un **mandat** est signé une fois par l'utilisateur, depuis l'app. C'est à ce
moment-là — et seulement là — que l'autorisation anti-fraude est demandée, par
l'API Node, sur le chemin normal (`transactionAuthorizationService` → moteur
Rust). Les prélèvements suivants ne repassent pas par le moteur de risque : ce
n'est plus un achat, c'est l'exécution d'un mandat déjà autorisé.

Le seul contrôle conservé à chaque cycle est `user_wallets.is_locked`. C'est une
lecture d'une colonne, et c'est le bouton qui permet à un Gardien d'arrêter net
un mandat qui s'emballe.

### Pourquoi l'abonnement n'expire jamais

`users.subscription_expires_at = NULL` signifie déjà « pas d'expiration » dans
toute la pile existante :

| Endroit | Comportement sur NULL |
|---|---|
| `isSubscriptionActive()` | `if (!subscription_expires_at) return true` |
| `maybeExpireSubscription()` | `if (!subscription_expires_at) return false` |
| `expireDueSubscriptions()` | `AND subscription_expires_at IS NOT NULL` |

Un mandat vivant met donc cette colonne à NULL, et garde la vraie date de
facturation dans `subscription_mandates.next_charge_at`. Le balayage horaire de
l'API ne peut structurellement pas rétrograder un compte sous mandat — il n'y a
aucune course à arbitrer, et l'impayé « on lui laisse l'abonnement » ne coûte
rien puisqu'il n'y a rien à laisser tomber.

À l'annulation, on repose `subscription_expires_at = next_charge_at` : la
période déjà payée s'écoule, puis le balayage existant reprend la main.

## Cycle de vie d'un mandat

```
                 prélèvement OK
   ┌──────────────────────────────────────────┐
   │                                          │
   ▼                                          │
ACTIVE ──── solde insuffisant ────► DUNNING ──┤
   ▲          (réessai / 3 h)          │      │
   │                                   │      │
   │                        20e échec  │      │
   │                                   ▼      │
   └───────── rechargement ────────  GRACE ───┘
                                       │
                        30 jours écoulés
                                       ▼
                                   DEFAULTED
                      (retour en free + compte suspendu)
```

Pendant DUNNING et GRACE, l'abonnement reste actif et le palier est conservé.
Un simple rechargement du portefeuille ramène le mandat en ACTIVE au réessai
suivant, sans aucune intervention.

**Le passage en DEFAULTED sanctionne**, dans la même instruction SQL que le
changement d'état :

- `subscription_tier` revient à `free`, `premium` à faux et
  `subscription_expires_at` à `NOW()`. Cette dernière écriture n'est pas
  cosmétique : tant que le mandat vivait, la colonne valait NULL, et le
  balayage horaire de l'API ne rétrograde que les comptes dont l'expiration
  est **non nulle**. Sans elle, un impayé garderait son palier à vie.
- `is_suspended` passe à vrai, `suspended_until` reste NULL — une suspension
  sans terme, que seul un Gardien peut lever (`banMiddleware` ne relève
  automatiquement que les suspensions dont l'échéance est passée). La bascule
  prend effet en 30 s au plus, le TTL du cache de `globalBanMiddleware`.
- `ban_count` n'est **pas** incrémenté : il compte les violations des règles de
  la communauté, et à 5 il vaut bannissement définitif. Un impayé n'y entre pas.

Le compte suspendu garde l'accès à `/api/notifications` et `/api/auth/me` : il
peut lire la notification qui lui explique pourquoi. Il ne peut en revanche
plus recharger son portefeuille depuis l'app — la régularisation passe donc
forcément par un Gardien.

## Tarification

| Palier | Tarif | Conversion |
|---|---|---|
| Plus | 15 € | recalculée au cours du NF **à chaque échéance** |
| Pro | 30 € | idem |
| Ultra | 300 NF | prix fixe, insensible au cours |

Les deux arrondis de l'API sont reproduits dans l'ordre — dix-millième
(`nfAmountForEur`) puis centième (`roundTWC`) — pour que le démon prélève
exactement la même chose que l'achat manuel, au même cours.

## Ce que le démon écrit

Une seule transaction Postgres par prélèvement, tout ou rien :

| Table | Écriture |
|---|---|
| `user_wallets` | `balance -= p`, `total_spent += p` (compte) ; `balance += p`, `total_earned += p` (trésorerie) |
| `transactions` | `type='TRANSFER'`, `status='COMPLETED'`, hash de 32 octets, `amount_in_eur` calculé en NUMERIC |
| `users` | `subscription_tier`, `premium=true`, `subscription_expires_at=NULL`, `+5` crédits de génération |
| `subscription_mandates` | échéance suivante, compteurs remis à zéro, transaction liée |

Les portefeuilles sont verrouillés dans l'ordre trié `user_id:currency_id`,
comme `EconomyLedger.lockWallets` — verrouiller dans un autre sens
provoquerait un interblocage avec un achat manuel concurrent, que PostgreSQL
résout en tuant une des deux transactions.

Aucun `SELECT ... FOR UPDATE` n'est posé sur `users` : l'`UPDATE` prend
lui-même un verrou « no key », et c'est précisément un `FOR UPDATE` posé là qui
ferait attendre les écritures concurrentes sur le compte.

## Installation

```bash
sudo apt install build-essential libpq-dev
make
sudo make install

# La table des mandats — a jouer a la main : migrate.js n'est jamais execute
# au demarrage de l'API, et sync() ne pose ni index partiel ni contrainte.
psql "$PGURI" -f sql/001_subscription_mandates.sql

sudo install -D -m 0600 deploy/autorenew.env.example /etc/twitninf/autorenew.env
sudo $EDITOR /etc/twitninf/autorenew.env          # renseigner ARW_PGURI
sudo install -m 0644 deploy/twitninf-autorenew.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now twitninf-autorenew
journalctl -u twitninf-autorenew -f
```

### Première mise en service

Faire une passe en simulation avant d'activer le service. `ARW_DRY_RUN=1`
n'écrit **rien** : ni prélèvement, ni comptage d'échec, ni passage en défaut.

```bash
ARW_ONCE=1 ARW_DRY_RUN=1 ARW_VERBOSE=1 ARW_PGURI="..." ./twitninf-autorenew
```

`ARW_ONCE=1` fait une seule passe puis rend la main — c'est aussi le mode à
utiliser si on préfère un déclenchement par cron au service permanent.

## Essais

Les essais tournent contre un vrai PostgreSQL, avec les tables et les types
réels : rien n'est simulé, puisque c'est du code qui déplace de l'argent.

```bash
sudo service postgresql start
./test/run-tests.sh
```

Couvre : prélèvement réussi, conversion au cours, prix fixe Ultra, solde
insuffisant, portefeuille gelé, échéance non atteinte, franchissement du seuil
de mise en demeure, délai épuisé, absence de double prélèvement, retour à la
normale après rechargement, et neutralité complète de la simulation.

## Ce qui reste à faire côté Node / mobile

Le démon exécute des mandats ; il n'en crée aucun. Trois pièces manquent, hors
du périmètre de ce dépôt :

1. **La route d'opt-in.** Elle doit prendre l'autorisation anti-fraude par le
   chemin normal, puis insérer le mandat et basculer le compte :

   ```sql
   INSERT INTO subscription_mandates
     (id, user_id, tier, state, currency_id, authorization_id, next_charge_at)
   VALUES (:id, :userId, :tier, 'ACTIVE', :currencyId, :authorizationId,
           COALESCE(:currentExpiry, NOW()));

   UPDATE users SET subscription_expires_at = NULL WHERE id = :userId;
   ```

   `next_charge_at` part de la fin de la période déjà payée, pas de maintenant :
   sinon la signature du mandat facture immédiatement du temps déjà acheté.

2. **La route d'annulation.** Elle repose l'expiration pour que la période
   payée s'écoule normalement :

   ```sql
   UPDATE users u SET subscription_expires_at = m.next_charge_at
     FROM subscription_mandates m
    WHERE m.user_id = u.id AND m.id = :mandateId;

   UPDATE subscription_mandates
      SET state = 'CANCELLED', cancelled_at = NOW(), next_retry_at = NULL
    WHERE id = :mandateId;
   ```

3. **L'affichage.** `premiumRoutes.js` renvoie `subscription_expires_at` tel
   quel : un compte sous mandat affichera « pas d'expiration » là où il
   faudrait « se renouvelle le ... ». La date à montrer est
   `subscription_mandates.next_charge_at`.

Le passage en `DEFAULTED` écrit dans `users` (palier, `premium`,
`subscription_expires_at`, `is_suspended`, `suspended_at`, `suspension_reason`,
`suspension_meta`) sans passer par Sequelize : les hooks du modèle ne
s'exécutent pas, chaque colonne dérivée est donc écrite à la main.

## Configuration

Tout passe par l'environnement, voir `deploy/autorenew.env.example`. Seule
`ARW_PGURI` est obligatoire ; les autres valeurs par défaut sont alignées sur
la configuration de l'API (période de 5 jours, trésorerie, tarifs, 5 crédits).
