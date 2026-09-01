#!/usr/bin/env bash
#
# Essais de bout en bout du démon, contre un vrai PostgreSQL.
#
# Le code déplace de l'argent : rien ici ne simule la base. On crée les tables
# réelles (types compris), on sème des cas, on lance le binaire, et on relit ce
# qu'il a écrit.
#
#   sudo service postgresql start
#   ./test/run-tests.sh
#
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/twitninf-autorenew"

DB=${ARW_TEST_DB:-arwtest}
DBUSER=${ARW_TEST_USER:-arwtest}
DBPASS=${ARW_TEST_PASS:-arwtest}
export ARW_PGURI="postgresql://$DBUSER:$DBPASS@127.0.0.1:5432/$DB"

TREASURY="01837802-c2ae-4de0-9471-7a9b642afde2"
CURRENCY="11111111-1111-4111-8111-111111111111"
U_PLUS="aaaaaaaa-0000-4000-8000-000000000001"
U_POOR="aaaaaaaa-0000-4000-8000-000000000002"
U_LOCK="aaaaaaaa-0000-4000-8000-000000000003"
U_ULTRA="aaaaaaaa-0000-4000-8000-000000000004"
U_LATER="aaaaaaaa-0000-4000-8000-000000000005"
U_GRACE="aaaaaaaa-0000-4000-8000-000000000006"
U_BRINK="aaaaaaaa-0000-4000-8000-000000000007"

PASSED=0
FAILED=0

psql_root() { sudo -n -u postgres psql -v ON_ERROR_STOP=1 -qtA "$@"; }
q() { PGPASSWORD="$DBPASS" psql -h 127.0.0.1 -U "$DBUSER" -d "$DB" -v ON_ERROR_STOP=1 -qtA -c "$1"; }

check() {
    local label="$1" expected="$2" actual="$3"
    if [ "$expected" = "$actual" ]; then
        printf '  \033[32mOK\033[0m   %s\n' "$label"
        PASSED=$((PASSED + 1))
    else
        printf '  \033[31mKO\033[0m   %s\n       attendu [%s], obtenu [%s]\n' \
               "$label" "$expected" "$actual"
        FAILED=$((FAILED + 1))
    fi
}

# ─── Préparation ────────────────────────────────────────────────────────────
echo "== preparation de la base d'essai =="
psql_root -c "DROP DATABASE IF EXISTS $DB" >/dev/null
psql_root -c "DO \$\$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname='$DBUSER') THEN CREATE ROLE $DBUSER LOGIN PASSWORD '$DBPASS'; END IF; END \$\$;" >/dev/null
psql_root -c "CREATE DATABASE $DB OWNER $DBUSER" >/dev/null

PGPASSWORD="$DBPASS" psql -h 127.0.0.1 -U "$DBUSER" -d "$DB" -v ON_ERROR_STOP=1 -q \
    -f "$ROOT/test/schema.sql" || exit 1
PGPASSWORD="$DBPASS" psql -h 127.0.0.1 -U "$DBUSER" -d "$DB" -v ON_ERROR_STOP=1 -q \
    -f "$ROOT/sql/001_subscription_mandates.sql" || exit 1

q "
INSERT INTO virtual_currencies (id, symbol, current_price, is_active)
VALUES ('$CURRENCY', 'NF', 10.0000, true);

INSERT INTO users (id, username, subscription_tier, subscription_expires_at, premium, tweet_generation_credits, g_auth_sub) VALUES
  ('$TREASURY', 'treasury', 'free',  NULL,           false, 0, NULL),
  ('$U_PLUS',   'plus',     'plus',  NULL,           true,  0, 'g|plus'),
  ('$U_POOR',   'poor',     'pro',   NULL,           true,  2, 'g|poor'),
  ('$U_LOCK',   'locked',   'plus',  NULL,           true,  0, 'g|lock'),
  ('$U_ULTRA',  'ultra',    'ultra', NULL,           true,  0, 'g|ultra'),
  ('$U_LATER',  'later',    'plus',  NULL,           true,  0, 'g|later'),
  ('$U_GRACE',  'grace',    'pro',   NULL,           true,  0, 'g|grace'),
  ('$U_BRINK',  'brink',    'plus',  NULL,           true,  0, 'g|brink');

INSERT INTO user_wallets (id, user_id, currency_id, balance, is_locked) VALUES
  (gen_random_uuid(), '$TREASURY', '$CURRENCY',   0.00, false),
  (gen_random_uuid(), '$U_PLUS',   '$CURRENCY', 100.00, false),
  (gen_random_uuid(), '$U_POOR',   '$CURRENCY',   0.50, false),
  (gen_random_uuid(), '$U_LOCK',   '$CURRENCY', 100.00, true),
  (gen_random_uuid(), '$U_ULTRA',  '$CURRENCY', 500.00, false),
  (gen_random_uuid(), '$U_LATER',  '$CURRENCY', 100.00, false),
  (gen_random_uuid(), '$U_BRINK',  '$CURRENCY',   0.10, false);

INSERT INTO subscription_mandates (id, user_id, tier, state, currency_id, next_charge_at, next_retry_at, failure_count, grace_until) VALUES
  (gen_random_uuid(), '$U_PLUS',  'plus',  'ACTIVE',  '$CURRENCY', NOW() - INTERVAL '1 minute', NULL, 0, NULL),
  (gen_random_uuid(), '$U_POOR',  'pro',   'ACTIVE',  '$CURRENCY', NOW() - INTERVAL '1 minute', NULL, 0, NULL),
  (gen_random_uuid(), '$U_LOCK',  'plus',  'ACTIVE',  '$CURRENCY', NOW() - INTERVAL '1 minute', NULL, 0, NULL),
  (gen_random_uuid(), '$U_ULTRA', 'ultra', 'ACTIVE',  '$CURRENCY', NOW() - INTERVAL '1 minute', NULL, 0, NULL),
  (gen_random_uuid(), '$U_LATER', 'plus',  'ACTIVE',  '$CURRENCY', NOW() + INTERVAL '2 days',   NULL, 0, NULL),
  (gen_random_uuid(), '$U_GRACE', 'pro',   'GRACE',   '$CURRENCY', NOW() + INTERVAL '9 days', NOW() + INTERVAL '3 hours', 20, NOW() - INTERVAL '1 hour'),
  (gen_random_uuid(), '$U_BRINK', 'plus',  'DUNNING', '$CURRENCY', NOW() - INTERVAL '3 days', NOW() - INTERVAL '1 minute', 19, NULL);
" >/dev/null || exit 1

# ─── Passe 1 ────────────────────────────────────────────────────────────────
echo
echo "== passe 1 : prelevements dus =="
ARW_ONCE=1 "$BIN" 2>&1 | sed 's/^/  | /'

echo
echo "-- prelevement reussi (Plus, 15 EUR au cours de 10 EUR = 1,50 NF)"
check "solde debite"            "98.50000000"  "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_PLUS'")"
check "total_spent mis a jour"  "1.50000000"   "$(q "SELECT total_spent FROM user_wallets WHERE user_id='$U_PLUS'")"
check "montant en euros exact"  "15.00000000"  "$(q "SELECT amount_in_eur FROM transactions WHERE from_user_id='$U_PLUS'")"
check "type de transaction"     "TRANSFER"     "$(q "SELECT type FROM transactions WHERE from_user_id='$U_PLUS'")"
check "statut de transaction"   "COMPLETED"    "$(q "SELECT status FROM transactions WHERE from_user_id='$U_PLUS'")"
check "hash sur 64 caracteres"  "64"           "$(q "SELECT length(transaction_hash) FROM transactions WHERE from_user_id='$U_PLUS'")"
check "marque autorenew"        "true"         "$(q "SELECT metadata->>'autorenew' FROM transactions WHERE from_user_id='$U_PLUS'")"
check "categorie de depense"    "Abonnement"   "$(q "SELECT metadata->>'spendingCategory' FROM transactions WHERE from_user_id='$U_PLUS'")"

echo
echo "-- l'abonnement n'expire jamais sous mandat"
check "expiration a NULL"       "t"            "$(q "SELECT subscription_expires_at IS NULL FROM users WHERE id='$U_PLUS'")"
check "premium conserve"        "t"            "$(q "SELECT premium FROM users WHERE id='$U_PLUS'")"
check "palier conserve"         "plus"         "$(q "SELECT subscription_tier FROM users WHERE id='$U_PLUS'")"
check "credits recharges (+5)"  "5"            "$(q "SELECT tweet_generation_credits FROM users WHERE id='$U_PLUS'")"
check "hors du balayage horaire" "0"           "$(q "SELECT count(*) FROM users WHERE id='$U_PLUS' AND subscription_tier <> 'free' AND subscription_expires_at IS NOT NULL AND subscription_expires_at <= NOW()")"

echo
echo "-- le mandat repart pour une periode"
check "etat ACTIVE"             "ACTIVE"       "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_PLUS'")"
check "compteur d'echecs remis" "0"            "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_PLUS'")"
check "echeance a +5 jours"     "t"            "$(q "SELECT next_charge_at BETWEEN NOW() + INTERVAL '4 days 23 hours' AND NOW() + INTERVAL '5 days 1 hour' FROM subscription_mandates WHERE user_id='$U_PLUS'")"
check "transaction liee"        "t"            "$(q "SELECT last_charge_tx IS NOT NULL FROM subscription_mandates WHERE user_id='$U_PLUS'")"

echo
echo "-- Ultra : prix en NF fixe, insensible au cours"
check "300 NF preleves"         "300.00000000" "$(q "SELECT amount FROM transactions WHERE from_user_id='$U_ULTRA'")"
check "solde Ultra"             "200.00000000" "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_ULTRA'")"

echo
echo "-- tresorerie creditee de la somme des deux"
check "solde tresorerie"        "301.50000000" "$(q "SELECT balance FROM user_wallets WHERE user_id='$TREASURY'")"
check "total_earned tresorerie" "301.50000000" "$(q "SELECT total_earned FROM user_wallets WHERE user_id='$TREASURY'")"

echo
echo "-- solde insuffisant : impaye, mais l'abonnement reste"
check "etat DUNNING"            "DUNNING"      "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_POOR'")"
check "un echec compte"         "1"            "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_POOR'")"
check "reessai a +3h"           "t"            "$(q "SELECT next_retry_at BETWEEN NOW() + INTERVAL '2 hours 55 minutes' AND NOW() + INTERVAL '3 hours 5 minutes' FROM subscription_mandates WHERE user_id='$U_POOR'")"
check "aucun NF deplace"        "0.50000000"   "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_POOR'")"
check "aucune transaction"      "0"            "$(q "SELECT count(*) FROM transactions WHERE from_user_id='$U_POOR'")"
check "palier conserve"         "pro"          "$(q "SELECT subscription_tier FROM users WHERE id='$U_POOR'")"
check "expiration toujours NULL" "t"           "$(q "SELECT subscription_expires_at IS NULL FROM users WHERE id='$U_POOR'")"
check "credits non recharges"   "2"            "$(q "SELECT tweet_generation_credits FROM users WHERE id='$U_POOR'")"
check "avertissement envoye"    "high"         "$(q "SELECT priority FROM notifications WHERE recipient_id='$U_POOR'")"

echo
echo "-- portefeuille gele : traite comme un impaye, pas comme une panne"
check "etat DUNNING"            "DUNNING"      "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_LOCK'")"
check "solde intact"            "100.00000000" "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_LOCK'")"
check "aucune transaction"      "0"            "$(q "SELECT count(*) FROM transactions WHERE from_user_id='$U_LOCK'")"

echo
echo "-- echeance non atteinte : rien ne bouge"
check "etat inchange"           "ACTIVE"       "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_LATER'")"
check "aucun echec"             "0"            "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_LATER'")"
check "aucune transaction"      "0"            "$(q "SELECT count(*) FROM transactions WHERE from_user_id='$U_LATER'")"

echo
echo "-- franchissement du seuil : mise en demeure"
check "etat GRACE"              "GRACE"        "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_BRINK'")"
check "20e echec"               "20"           "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_BRINK'")"
check "delai d'un mois pose"    "t"            "$(q "SELECT grace_until BETWEEN NOW() + INTERVAL '29 days' AND NOW() + INTERVAL '31 days' FROM subscription_mandates WHERE user_id='$U_BRINK'")"
check "notification urgente"    "urgent"       "$(q "SELECT priority FROM notifications WHERE recipient_id='$U_BRINK'")"
check "une seule notification"  "1"            "$(q "SELECT count(*) FROM notifications WHERE recipient_id='$U_BRINK'")"
check "abonnement conserve"     "t"            "$(q "SELECT subscription_expires_at IS NULL AND premium FROM users WHERE id='$U_BRINK'")"
check "pas encore suspendu"     "f"            "$(q "SELECT is_suspended FROM users WHERE id='$U_BRINK'")"

echo
echo "-- delai epuise : abonnement repris et compte suspendu"
check "etat DEFAULTED"          "DEFAULTED"    "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_GRACE'")"
check "notification urgente"    "urgent"       "$(q "SELECT priority FROM notifications WHERE recipient_id='$U_GRACE'")"
check "plus de reessai"         "t"            "$(q "SELECT next_retry_at IS NULL FROM subscription_mandates WHERE user_id='$U_GRACE'")"
check "retour en free"          "free"         "$(q "SELECT subscription_tier FROM users WHERE id='$U_GRACE'")"
check "premium retire"          "f"            "$(q "SELECT premium FROM users WHERE id='$U_GRACE'")"
check "expiration posee"        "t"            "$(q "SELECT subscription_expires_at IS NOT NULL AND subscription_expires_at <= NOW() FROM users WHERE id='$U_GRACE'")"
check "compte suspendu"         "t"            "$(q "SELECT is_suspended FROM users WHERE id='$U_GRACE'")"
check "suspension sans terme"   "t"            "$(q "SELECT suspended_at IS NOT NULL AND suspended_until IS NULL FROM users WHERE id='$U_GRACE'")"
check "motif de suspension"     "t"            "$(q "SELECT suspension_reason LIKE '%defaut de paiement%' FROM users WHERE id='$U_GRACE'")"
check "mandat trace en meta"    "twitninf-autorenew" "$(q "SELECT suspension_meta->>'source' FROM users WHERE id='$U_GRACE'")"
check "ban_count intact"        "0"            "$(q "SELECT ban_count FROM users WHERE id='$U_GRACE'")"

# ─── Passe 2 : idempotence ──────────────────────────────────────────────────
echo
echo "== passe 2 : relance immediate (aucun double prelevement) =="
ARW_ONCE=1 "$BIN" 2>&1 | sed 's/^/  | /'

check "toujours 2 transactions" "2"            "$(q "SELECT count(*) FROM transactions")"
check "solde Plus inchange"     "98.50000000"  "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_PLUS'")"
check "tresorerie inchangee"    "301.50000000" "$(q "SELECT balance FROM user_wallets WHERE user_id='$TREASURY'")"
check "echec Poor non recompte" "1"            "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_POOR'")"

# ─── Passe 3 : le rechargement sauve le mandat tout seul ────────────────────
echo
echo "== passe 3 : rechargement du portefeuille en impaye =="
q "UPDATE user_wallets SET balance = 50.00 WHERE user_id='$U_POOR';
   UPDATE subscription_mandates SET next_retry_at = NOW() - INTERVAL '1 minute' WHERE user_id='$U_POOR';" >/dev/null
ARW_ONCE=1 "$BIN" 2>&1 | sed 's/^/  | /'

check "retour en ACTIVE"        "ACTIVE"       "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_POOR'")"
check "compteur remis a zero"   "0"            "$(q "SELECT failure_count FROM subscription_mandates WHERE user_id='$U_POOR'")"
check "3 NF preleves (Pro)"     "47.00000000"  "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_POOR'")"
check "credits recharges"       "7"            "$(q "SELECT tweet_generation_credits FROM users WHERE id='$U_POOR'")"
check "aucun reessai en attente" "t"           "$(q "SELECT next_retry_at IS NULL FROM subscription_mandates WHERE user_id='$U_POOR'")"

# ─── Passe 4 : le cours bouge, le prix en euros ne bouge pas ────────────────
echo
echo "== passe 4 : cours du NF a 7,50 EUR (Plus doit couter 2,00 NF) =="
q "UPDATE virtual_currencies SET current_price = 7.5000 WHERE id='$CURRENCY';
   UPDATE subscription_mandates SET next_charge_at = NOW() - INTERVAL '1 minute' WHERE user_id='$U_PLUS';" >/dev/null
ARW_ONCE=1 "$BIN" 2>&1 | sed 's/^/  | /'

check "2,00 NF preleves"        "2.00000000"   "$(q "SELECT amount FROM transactions WHERE from_user_id='$U_PLUS' ORDER BY created_at DESC LIMIT 1")"
check "toujours 15 EUR"         "15.00000000"  "$(q "SELECT amount_in_eur FROM transactions WHERE from_user_id='$U_PLUS' ORDER BY created_at DESC LIMIT 1")"
check "solde apres"             "96.50000000"  "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_PLUS'")"

echo
echo "== simulation (ARW_DRY_RUN) : aucune ecriture, d'aucune sorte =="
# Trois chemins d'ecriture doivent etre neutralises en meme temps : le
# prelevement reussi, l'enregistrement d'un impaye (qui survient AVANT le
# prelevement, donc ne peut pas etre couvert par la meme garde), et le passage
# en defaut d'un delai epuise.
q "UPDATE subscription_mandates SET next_charge_at = NOW() - INTERVAL '1 minute' WHERE user_id IN ('$U_ULTRA', '$U_PLUS');
   UPDATE subscription_mandates SET state = 'GRACE', grace_until = NOW() - INTERVAL '1 hour',
          next_retry_at = NOW() - INTERVAL '1 minute'
    WHERE user_id = '$U_LATER';" >/dev/null

BEFORE_TX="$(q "SELECT count(*) FROM transactions")"
BEFORE_NOTIF="$(q "SELECT count(*) FROM notifications")"
BEFORE_FAILS="$(q "SELECT coalesce(sum(failure_count), 0) FROM subscription_mandates")"

ARW_ONCE=1 ARW_DRY_RUN=1 "$BIN" 2>&1 | sed 's/^/  | /'

check "aucune transaction ajoutee"  "$BEFORE_TX"    "$(q "SELECT count(*) FROM transactions")"
check "aucune notification envoyee" "$BEFORE_NOTIF" "$(q "SELECT count(*) FROM notifications")"
check "aucun echec compte"          "$BEFORE_FAILS" "$(q "SELECT coalesce(sum(failure_count), 0) FROM subscription_mandates")"
check "solde suffisant non debite"  "96.50000000"   "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_PLUS'")"
check "solde insuffisant intact"    "200.00000000"  "$(q "SELECT balance FROM user_wallets WHERE user_id='$U_ULTRA'")"
check "impaye non enregistre"       "ACTIVE"        "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_ULTRA'")"
check "delai epuise non traite"     "GRACE"         "$(q "SELECT state FROM subscription_mandates WHERE user_id='$U_LATER'")"
check "aucune suspension"           "f"             "$(q "SELECT is_suspended FROM users WHERE id='$U_LATER'")"
check "abonnement non repris"       "t"             "$(q "SELECT subscription_expires_at IS NULL AND premium FROM users WHERE id='$U_LATER'")"

echo
echo "─────────────────────────────────────────"
printf "  %d reussis, %d echoues\n" "$PASSED" "$FAILED"
[ "$FAILED" -eq 0 ] || exit 1
