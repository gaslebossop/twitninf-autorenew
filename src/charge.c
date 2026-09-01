#include "charge.h"
#include "db.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Catégorie de dépense de l'API pour `subscription_purchase`
 * (newEconomyService.getSpendingCategory). Reprise telle quelle pour que les
 * renouvellements se rangent avec les achats manuels dans les statistiques. */
#define ARW_SPENDING_CATEGORY "Abonnement"

static void fail(arw_charge_result *out, arw_charge_status status, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void fail(arw_charge_result *out, arw_charge_status status, const char *fmt, ...)
{
    out->status = status;
    va_list args;
    va_start(args, fmt);
    vsnprintf(out->detail, sizeof(out->detail), fmt, args);
    va_end(args);
}

/*
 * Verrouille un portefeuille, en le créant s'il n'existe pas.
 *
 * L'upsert reprend `ON CONFLICT DO NOTHING` plutôt qu'un findOrCreate : c'est
 * le motif déjà retenu côté API après une série d'états « transaction avortée »
 * (25P02) sous concurrence sur le portefeuille trésorerie, très sollicité.
 *
 * `price` sert à faire trancher la comparaison de solde par Postgres, en
 * NUMERIC exact, plutôt que par un double C.
 */
static PGresult *lock_wallet(PGconn *conn, const char *user_id, const char *currency_id,
                             const char *price)
{
    arw_uuid fresh_id;
    if (!arw_db_uuid4(fresh_id)) {
        return NULL;
    }

    static const char *upsert =
        "INSERT INTO user_wallets "
        "  (id, user_id, currency_id, balance, total_earned, total_spent, "
        "   total_purchased, loyalty_points, is_locked, created_at, updated_at) "
        "VALUES ($1, $2, $3, 0, 0, 0, 0, 0, false, NOW(), NOW()) "
        "ON CONFLICT (user_id, currency_id) DO NOTHING";

    const char *up_params[3] = { fresh_id, user_id, currency_id };
    if (arw_db_command(conn, upsert, 3, up_params) < 0) {
        return NULL;
    }

    static const char *select =
        "SELECT balance::text, is_locked, (balance >= $3::numeric) AS sufficient "
        "  FROM user_wallets "
        " WHERE user_id = $1 AND currency_id = $2 "
        "   FOR UPDATE";

    const char *sel_params[3] = { user_id, currency_id, price };
    PGresult *res = arw_db_query(conn, select, 3, sel_params);
    if (res && PQntuples(res) != 1) {
        LOG_ERROR("portefeuille introuvable apres creation : user=%s", user_id);
        PQclear(res);
        return NULL;
    }
    return res;
}

arw_charge_result arw_charge_run(PGconn *conn, const arw_config *cfg,
                                 const char *mandate_id)
{
    arw_charge_result out;
    memset(&out, 0, sizeof(out));
    out.status = ARW_CHARGE_ERROR;

    if (!arw_db_begin(conn)) {
        fail(&out, ARW_CHARGE_ERROR, "BEGIN refuse");
        return out;
    }

    /* --- 1. Le mandat, verrouille -------------------------------------
     * La condition d'échéance est REVÉRIFIÉE ici, sous verrou : la sélection
     * en amont travaille sans verrou, donc une autre passe a pu traiter ce
     * mandat entre-temps. Sans cette revérification, un mandat pourrait être
     * prélevé deux fois pour la même période. */
    static const char *sql_mandate =
        "SELECT user_id, tier, state, currency_id "
        "  FROM subscription_mandates "
        " WHERE id = $1 "
        "   AND state IN ('ACTIVE', 'DUNNING', 'GRACE') "
        "   AND ( (state = 'ACTIVE' AND next_charge_at <= NOW()) "
        "      OR (state IN ('DUNNING', 'GRACE') AND next_retry_at <= NOW()) ) "
        "   FOR UPDATE";

    const char *m_params[1] = { mandate_id };
    PGresult *mandate = arw_db_query(conn, sql_mandate, 1, m_params);
    if (!mandate) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "lecture du mandat impossible");
        return out;
    }
    if (PQntuples(mandate) == 0) {
        PQclear(mandate);
        arw_db_rollback(conn);
        out.status = ARW_CHARGE_SKIPPED;
        return out;
    }

    snprintf(out.user_id, sizeof(out.user_id), "%s", PQgetvalue(mandate, 0, 0));
    snprintf(out.tier, sizeof(out.tier), "%s", PQgetvalue(mandate, 0, 1));
    char currency_id[ARW_UUID_LEN + 1];
    snprintf(currency_id, sizeof(currency_id), "%s", PQgetvalue(mandate, 0, 3));
    PQclear(mandate);

    /* Un mandat sur la trésorerie elle-même se débiterait et se créditerait sur
     * la même ligne : le ledger de l'API refuse ce cas, on le refuse aussi. */
    if (strcmp(out.user_id, cfg->treasury_user_id) == 0) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "mandat pose sur le compte de tresorerie");
        return out;
    }

    /* --- 2. Cours du NF ------------------------------------------------
     * Relu à CHAQUE prélèvement, dans la transaction : Plus et Pro sont
     * tarifés en euros, donc le nombre de NF débité suit le cours du jour. */
    static const char *sql_currency =
        "SELECT current_price::text FROM virtual_currencies "
        " WHERE id = $1 AND is_active = true";

    const char *c_params[1] = { currency_id };
    PGresult *currency = arw_db_query(conn, sql_currency, 1, c_params);
    if (!currency || PQntuples(currency) != 1) {
        if (currency) PQclear(currency);
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "monnaie indisponible ou inactive");
        return out;
    }
    char price_eur_text[64];
    snprintf(price_eur_text, sizeof(price_eur_text), "%s", PQgetvalue(currency, 0, 0));
    double price_eur = strtod(price_eur_text, NULL);
    PQclear(currency);

    /* --- 3. Prix de la periode, en NF ---------------------------------- */
    double price_nf = arw_config_price_nf(cfg, out.tier, price_eur);
    if (price_nf <= 0.0) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "prix incalculable (palier %s, cours %s)",
             out.tier, price_eur_text);
        return out;
    }
    snprintf(out.amount_nf, sizeof(out.amount_nf), "%.2f", price_nf);

    /* --- 4. Portefeuilles, verrouilles dans l'ordre trie ----------------
     * L'ordre est celui de la clé "user_id:currency_id", jamais l'ordre
     * naturel compte -> trésorerie. Les deux portefeuilles partagent la même
     * monnaie, la comparaison se réduit donc aux identifiants de compte.
     * Verrouiller toujours dans le même sens que l'API évite l'interblocage
     * qu'un achat manuel concurrent déclencherait autrement — PostgreSQL le
     * résout en tuant une des deux transactions, c'est-à-dire un prélèvement
     * qui échoue sans raison lisible. */
    bool user_first = strcmp(out.user_id, cfg->treasury_user_id) < 0;
    const char *first  = user_first ? out.user_id : cfg->treasury_user_id;
    const char *second = user_first ? cfg->treasury_user_id : out.user_id;

    PGresult *w_first = lock_wallet(conn, first, currency_id, out.amount_nf);
    if (!w_first) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "verrou portefeuille (%s) impossible", first);
        return out;
    }
    PGresult *w_second = lock_wallet(conn, second, currency_id, out.amount_nf);
    if (!w_second) {
        PQclear(w_first);
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "verrou portefeuille (%s) impossible", second);
        return out;
    }

    PGresult *user_wallet = user_first ? w_first : w_second;
    snprintf(out.balance, sizeof(out.balance), "%s", PQgetvalue(user_wallet, 0, 0));
    bool is_locked  = (strcmp(PQgetvalue(user_wallet, 0, 1), "t") == 0);
    bool sufficient = (strcmp(PQgetvalue(user_wallet, 0, 2), "t") == 0);
    PQclear(w_first);
    PQclear(w_second);

    /* --- 5. Les deux seuls refus possibles -----------------------------
     * Un portefeuille gelé compte comme un impayé, pas comme une panne : le
     * mandat entre en relance, la personne est prévenue, et un dégel la
     * ramène toute seule en ACTIVE au réessai suivant. */
    if (is_locked) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_WALLET_LOCKED, "portefeuille gele");
        return out;
    }
    if (!sufficient) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_INSUFFICIENT, "solde %s NF pour %s NF dus",
             out.balance, out.amount_nf);
        return out;
    }

    if (cfg->dry_run) {
        arw_db_rollback(conn);
        out.status = ARW_CHARGE_SKIPPED;
        snprintf(out.detail, sizeof(out.detail),
                 "[SIMULATION] %s NF auraient ete preleves (palier %s)",
                 out.amount_nf, out.tier);
        return out;
    }

    /* --- 6. Debit du compte, credit de la tresorerie --------------------
     * `ROUND(..., 2)` reproduit `roundTWC` du ledger : la colonne est en
     * NUMERIC(20,8) mais toute l'économie travaille à deux décimales. */
    static const char *sql_debit =
        "UPDATE user_wallets "
        "   SET balance     = ROUND(balance - $3::numeric, 2), "
        "       total_spent = ROUND(total_spent + $3::numeric, 2), "
        "       updated_at  = NOW() "
        " WHERE user_id = $1 AND currency_id = $2";

    const char *debit_params[3] = { out.user_id, currency_id, out.amount_nf };
    if (arw_db_command(conn, sql_debit, 3, debit_params) != 1) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "debit refuse");
        return out;
    }

    static const char *sql_credit =
        "UPDATE user_wallets "
        "   SET balance      = ROUND(balance + $3::numeric, 2), "
        "       total_earned = ROUND(total_earned + $3::numeric, 2), "
        "       updated_at   = NOW() "
        " WHERE user_id = $1 AND currency_id = $2";

    const char *credit_params[3] = { cfg->treasury_user_id, currency_id, out.amount_nf };
    if (arw_db_command(conn, sql_credit, 3, credit_params) != 1) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "credit tresorerie refuse");
        return out;
    }

    /* --- 7. La ligne de grand livre -------------------------------------
     * `amount_in_eur` est calculé par Postgres en NUMERIC plutôt qu'en double
     * C : c'est la valorisation de l'écriture, elle doit être exacte.
     * `itemType` reste `subscription_purchase` pour que ces débits se rangent
     * avec les achats manuels dans les statistiques ; `autorenew` les en
     * distingue quand on veut les isoler. */
    arw_uuid tx_id;
    char tx_hash[ARW_HASH_LEN + 1];
    if (!arw_db_uuid4(tx_id) || !arw_db_tx_hash(tx_hash)) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "entropie indisponible");
        return out;
    }

    char description[160];
    snprintf(description, sizeof(description),
             "Renouvellement automatique - abonnement %s", out.tier);

    char metadata[512];
    snprintf(metadata, sizeof(metadata),
             "{\"itemType\":\"subscription_purchase\","
             "\"itemId\":\"%s\","
             "\"spendingCategory\":\"%s\","
             "\"ledger\":\"SPEND_TO_TREASURY\","
             "\"autorenew\":true,"
             "\"mandateId\":\"%s\","
             "\"riskAuthorization\":\"mandate\"}",
             out.tier, ARW_SPENDING_CATEGORY, mandate_id);

    static const char *sql_tx =
        "INSERT INTO transactions "
        "  (id, transaction_hash, from_user_id, to_user_id, currency_id, "
        "   amount, amount_in_eur, type, status, fee, description, metadata, "
        "   confirmed_at, created_at, updated_at) "
        "VALUES ($1, $2, $3, $4, $5, "
        "        $6::numeric, ROUND($6::numeric * $7::numeric, 2), "
        "        'TRANSFER', 'COMPLETED', 0, $8, $9, "
        "        NOW(), NOW(), NOW())";

    const char *tx_params[9] = {
        tx_id, tx_hash, out.user_id, cfg->treasury_user_id, currency_id,
        out.amount_nf, price_eur_text, description, metadata
    };
    if (arw_db_command(conn, sql_tx, 9, tx_params) != 1) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "ecriture du grand livre refusee");
        return out;
    }
    snprintf(out.tx_id, sizeof(out.tx_id), "%s", tx_id);

    /* --- 8. L'abonnement ------------------------------------------------
     * `subscription_expires_at = NULL` est l'invariant du mandat : le compte
     * ne peut plus être rétrogradé par le balayage horaire de l'API, qui
     * filtre `AND subscription_expires_at IS NOT NULL`.
     *
     * `premium` est écrit explicitement : les hooks Sequelize qui le dérivent
     * du palier ne s'exécutent pas sur une écriture SQL directe.
     *
     * Aucun SELECT ... FOR UPDATE sur `users` : l'UPDATE prend lui-même un
     * verrou « no key », et c'est précisément un FOR UPDATE posé ici qui
     * ferait attendre les écritures concurrentes sur le compte.
     *
     * Les crédits de génération sont rechargés à chaque paiement confirmé,
     * renouvellement compris — même règle que l'achat manuel. */
    char credits[16];
    snprintf(credits, sizeof(credits), "%d", cfg->tweet_credits);

    static const char *sql_user =
        "UPDATE users "
        "   SET subscription_tier = $2, "
        "       premium = true, "
        "       subscription_expires_at = NULL, "
        "       tweet_generation_credits = "
        "         GREATEST(COALESCE(tweet_generation_credits, 0), 0) + $3::int, "
        "       updated_at = NOW() "
        " WHERE id = $1";

    const char *user_params[3] = { out.user_id, out.tier, credits };
    if (arw_db_command(conn, sql_user, 3, user_params) != 1) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "reconduction de l'abonnement refusee");
        return out;
    }

    /* --- 9. Le mandat repart pour une periode ---------------------------
     * `GREATEST(next_charge_at, NOW())` : une échéance traitée à l'heure
     * repart de sa date théorique, ce qui évite que le décalage du tick ne
     * dérive de passage en passage ; un mandat sorti d'un long impayé repart
     * de maintenant, et non rétroactivement. */
    char period[16];
    snprintf(period, sizeof(period), "%d", cfg->period_days);

    static const char *sql_mandate_ok =
        "UPDATE subscription_mandates "
        "   SET state          = 'ACTIVE', "
        "       next_charge_at = GREATEST(next_charge_at, NOW()) "
        "                        + ($2::int * INTERVAL '1 day'), "
        "       next_retry_at  = NULL, "
        "       failure_count  = 0, "
        "       grace_until    = NULL, "
        "       last_charge_at = NOW(), "
        "       last_charge_tx = $3, "
        "       last_error     = NULL, "
        "       updated_at     = NOW() "
        " WHERE id = $1";

    const char *ok_params[3] = { mandate_id, period, tx_id };
    if (arw_db_command(conn, sql_mandate_ok, 3, ok_params) != 1) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "avancement du mandat refuse");
        return out;
    }

    if (!arw_db_commit(conn)) {
        arw_db_rollback(conn);
        fail(&out, ARW_CHARGE_ERROR, "COMMIT refuse - aucun debit enregistre");
        return out;
    }

    out.status = ARW_CHARGE_OK;
    return out;
}
