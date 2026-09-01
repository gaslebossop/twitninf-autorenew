-- Mandats de renouvellement automatique des abonnements TwitNinf.
--
-- À jouer À LA MAIN sur la base de production : `migrate.js` n'est jamais
-- exécuté au démarrage de l'API, et `sync()` ne pose ni index partiel ni
-- contrainte de vérification. Rien d'autre ne créera cette table.
--
--     psql "$PGURI" -f sql/001_subscription_mandates.sql
--
-- ─────────────────────────────────────────────────────────────────────────────
-- INVARIANT CENTRAL — pourquoi un abonnement sous mandat n'expire jamais
--
-- `users.subscription_expires_at = NULL` signifie déjà « pas d'expiration »
-- dans toute la pile :
--   • isSubscriptionActive()   → `if (!user.subscription_expires_at) return true`
--   • maybeExpireSubscription()→ `if (!user.subscription_expires_at) return false`
--   • expireDueSubscriptions() → `AND subscription_expires_at IS NOT NULL`
--
-- Un mandat vivant met donc `subscription_expires_at` à NULL et garde la vraie
-- date de facturation ici, dans `next_charge_at`. Le balayage horaire de
-- l'API ne peut structurellement pas rétrograder un compte sous mandat — il
-- n'y a aucune course à arbitrer, et l'impayé « on lui laisse l'abonnement »
-- est gratuit puisqu'il n'y a rien à laisser tomber.
--
-- À l'annulation, on repose `subscription_expires_at = next_charge_at` : la
-- période déjà payée s'écoule, puis le balayage existant reprend la main.
-- ─────────────────────────────────────────────────────────────────────────────

BEGIN;

CREATE TABLE IF NOT EXISTS subscription_mandates (
  id                UUID PRIMARY KEY,
  user_id           UUID        NOT NULL REFERENCES users(id) ON DELETE CASCADE,

  -- Palier reconduit. Figé à la signature : une montée en gamme annule le
  -- mandat et en ouvre un neuf, avec sa propre autorisation anti-fraude.
  tier              VARCHAR(10) NOT NULL CHECK (tier IN ('plus', 'pro', 'ultra')),

  state             VARCHAR(12) NOT NULL DEFAULT 'ACTIVE'
                      CHECK (state IN ('ACTIVE', 'DUNNING', 'GRACE', 'DEFAULTED', 'CANCELLED')),

  currency_id       UUID        NOT NULL REFERENCES virtual_currencies(id),

  -- L'autorisation anti-fraude est prise UNE FOIS, à la signature, par l'API
  -- Node (chemin normal : transactionAuthorizationService → moteur Rust). Les
  -- prélèvements suivants ne repassent pas par le moteur de risque : c'est un
  -- mandat, pas un achat neuf. On conserve la trace de ce qui a été autorisé.
  authorized_at     TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  authorization_id  UUID        NULL REFERENCES transaction_risk_authorizations(id) ON DELETE SET NULL,

  next_charge_at    TIMESTAMPTZ NOT NULL,
  next_retry_at     TIMESTAMPTZ NULL,
  failure_count     INTEGER     NOT NULL DEFAULT 0,

  -- Posé au franchissement du seuil d'échecs : date limite de régularisation.
  grace_until       TIMESTAMPTZ NULL,

  last_charge_at    TIMESTAMPTZ NULL,
  last_charge_tx    UUID        NULL REFERENCES transactions(id) ON DELETE SET NULL,
  last_error        TEXT        NULL,

  cancelled_at      TIMESTAMPTZ NULL,
  created_at        TIMESTAMPTZ NOT NULL DEFAULT NOW(),
  updated_at        TIMESTAMPTZ NOT NULL DEFAULT NOW()
);

-- Un seul mandat vivant par compte. Partiel : l'historique des mandats
-- annulés reste consultable sans bloquer une nouvelle signature.
CREATE UNIQUE INDEX IF NOT EXISTS subscription_mandates_live_user
  ON subscription_mandates (user_id)
  WHERE state <> 'CANCELLED';

-- Les deux index de sélection du démon : échéances normales d'un côté,
-- réessais d'impayé de l'autre. Partiels, parce que les mandats DEFAULTED et
-- CANCELLED ne sont jamais relus par la boucle.
CREATE INDEX IF NOT EXISTS subscription_mandates_due
  ON subscription_mandates (next_charge_at)
  WHERE state = 'ACTIVE';

CREATE INDEX IF NOT EXISTS subscription_mandates_retry
  ON subscription_mandates (next_retry_at)
  WHERE state IN ('DUNNING', 'GRACE');

CREATE INDEX IF NOT EXISTS subscription_mandates_grace
  ON subscription_mandates (grace_until)
  WHERE state = 'GRACE';

COMMIT;
