#include "mandate.h"
#include "charge.h"
#include "db.h"
#include "log.h"
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *tier_label(const char *tier)
{
    if (strcmp(tier, "plus") == 0)  return "Plus";
    if (strcmp(tier, "pro") == 0)   return "Pro";
    if (strcmp(tier, "ultra") == 0) return "Ultra";
    return tier;
}

int arw_mandate_collect_due(PGconn *conn, const arw_config *cfg,
                            arw_uuid *ids, int max_ids)
{
    char limit[16];
    int wanted = (cfg->batch_size < max_ids) ? cfg->batch_size : max_ids;
    snprintf(limit, sizeof(limit), "%d", wanted);

    /* Les mandats en retard passent devant : un impayé qui attend depuis trois
     * heures est plus urgent qu'une échéance qui vient tout juste d'arriver. */
    static const char *sql =
        "SELECT id FROM subscription_mandates "
        " WHERE (state = 'ACTIVE' AND next_charge_at <= NOW()) "
        "    OR (state IN ('DUNNING', 'GRACE') AND next_retry_at <= NOW()) "
        " ORDER BY COALESCE(next_retry_at, next_charge_at) ASC "
        " LIMIT $1::int";

    const char *params[1] = { limit };
    PGresult *res = arw_db_query(conn, sql, 1, params);
    if (!res) {
        return -1;
    }

    int count = PQntuples(res);
    for (int i = 0; i < count; i++) {
        snprintf(ids[i], sizeof(ids[i]), "%s", PQgetvalue(res, i, 0));
    }
    PQclear(res);
    return count;
}

/*
 * Enregistre un échec et fait avancer la machine à états.
 *
 * Le franchissement du seuil est détecté par l'ÉGALITÉ `failure_count ==
 * max_failures`, pas par `>=` : la mise en demeure part ainsi exactement une
 * fois, au passage, et les réessais suivants ne la répètent pas. Le mandat
 * reste relancé pendant toute la période de grâce — un rechargement du
 * portefeuille suffit à le ramener en ACTIVE sans aucune intervention.
 *
 * L'abonnement, lui, ne bouge pas : `subscription_expires_at` reste NULL, donc
 * la personne garde son palier pendant tout l'impayé.
 */
static void record_failure(PGconn *conn, const arw_config *cfg,
                           const arw_charge_result *charge, const char *mandate_id)
{
    char retry[16], threshold[16], grace[16];
    snprintf(retry, sizeof(retry), "%d", cfg->retry_hours);
    snprintf(threshold, sizeof(threshold), "%d", cfg->max_failures);
    snprintf(grace, sizeof(grace), "%d", cfg->grace_days);

    static const char *sql =
        "UPDATE subscription_mandates "
        "   SET failure_count = failure_count + 1, "
        "       next_retry_at = NOW() + ($2::int * INTERVAL '1 hour'), "
        "       state = CASE WHEN failure_count + 1 >= $3::int "
        "                    THEN 'GRACE' ELSE 'DUNNING' END, "
        "       grace_until = CASE "
        "                       WHEN failure_count + 1 >= $3::int AND grace_until IS NULL "
        "                       THEN NOW() + ($4::int * INTERVAL '1 day') "
        "                       ELSE grace_until "
        "                     END, "
        "       last_error = $5, "
        "       updated_at = NOW() "
        " WHERE id = $1 "
        "   AND state IN ('ACTIVE', 'DUNNING', 'GRACE') "
        "RETURNING failure_count, state, "
        "          COALESCE(to_char(grace_until, 'DD/MM/YYYY'), '')";

    const char *params[5] = {
        mandate_id, retry, threshold, grace, charge->detail
    };

    PGresult *res = arw_db_query(conn, sql, 5, params);
    if (!res || PQntuples(res) != 1) {
        if (res) PQclear(res);
        LOG_ERROR("mandat %s : echec non enregistre", mandate_id);
        return;
    }

    int failures = atoi(PQgetvalue(res, 0, 0));
    char grace_date[32];
    snprintf(grace_date, sizeof(grace_date), "%s", PQgetvalue(res, 0, 2));
    PQclear(res);

    const char *label = tier_label(charge->tier);
    char title[128];
    char message[512];
    char content[512];

    if (failures == cfg->max_failures) {
        /* Mise en demeure : le palier est conservé, mais la régularisation a
         * désormais une date limite. C'est la seule notification urgente du
         * cycle — les échecs intermédiaires ne réveillent pas la personne
         * toutes les trois heures. */
        snprintf(title, sizeof(title), "Abonnement %s impaye", label);
        snprintf(message, sizeof(message),
                 "Nous n'avons pas pu prelever %s NF pour votre abonnement %s apres "
                 "%d tentatives. Votre abonnement reste actif, mais vous avez jusqu'au "
                 "%s pour recharger votre portefeuille.",
                 charge->amount_nf, label, failures, grace_date);
        snprintf(content, sizeof(content),
                 "{\"kind\":\"mandate_dunning_notice\",\"tier\":\"%s\","
                 "\"amount_nf\":\"%s\",\"failures\":%d,\"grace_until\":\"%s\"}",
                 charge->tier, charge->amount_nf, failures, grace_date);

        arw_notify(conn, charge->user_id, title, message, "urgent", content);
        LOG_WARN("mandat %s : mise en demeure envoyee (echec %d, limite %s)",
                 mandate_id, failures, grace_date);
        return;
    }

    if (failures == 1) {
        /* Premier échec : un avertissement simple, pour que la personne
         * recharge avant que la relance ne s'installe. Sans lui, le premier
         * signal reçu serait la mise en demeure, soixante heures plus tard. */
        snprintf(title, sizeof(title), "Renouvellement %s en attente", label);
        snprintf(message, sizeof(message),
                 "Votre solde ne couvre pas les %s NF du renouvellement %s. "
                 "Nous reessayons toutes les %d heures — rechargez votre portefeuille "
                 "et tout repart automatiquement.",
                 charge->amount_nf, label, cfg->retry_hours);
        snprintf(content, sizeof(content),
                 "{\"kind\":\"mandate_payment_failed\",\"tier\":\"%s\","
                 "\"amount_nf\":\"%s\",\"balance\":\"%s\"}",
                 charge->tier, charge->amount_nf, charge->balance);

        arw_notify(conn, charge->user_id, title, message, "high", content);
    }

    LOG_INFO("mandat %s : echec %d/%d — %s",
             mandate_id, failures, cfg->max_failures, charge->detail);
}

void arw_mandate_process(PGconn *conn, const arw_config *cfg,
                         const char *mandate_id, arw_pass_stats *stats)
{
    arw_charge_result charge = arw_charge_run(conn, cfg, mandate_id);

    switch (charge.status) {
    case ARW_CHARGE_OK:
        stats->charged++;
        LOG_INFO("mandat %s : %s NF preleves (palier %s, user %s, tx %s)",
                 mandate_id, charge.amount_nf, charge.tier,
                 charge.user_id, charge.tx_id);
        break;

    case ARW_CHARGE_INSUFFICIENT:
    case ARW_CHARGE_WALLET_LOCKED:
        stats->unpaid++;
        /* En simulation, un impayé se journalise mais ne s'enregistre pas :
         * sinon une passe d'essai ferait avancer les compteurs d'échec et
         * enverrait de vraies mises en demeure. Le prélèvement lui-même est
         * déjà neutralisé plus haut, mais le REFUS survient avant ce point —
         * il faut donc le neutraliser ici aussi. */
        if (cfg->dry_run) {
            LOG_INFO("[SIMULATION] mandat %s : %s (aucun echec enregistre)",
                     mandate_id, charge.detail);
            break;
        }
        record_failure(conn, cfg, &charge, mandate_id);
        break;

    case ARW_CHARGE_SKIPPED:
        stats->skipped++;
        if (charge.detail[0]) {
            LOG_INFO("mandat %s : %s", mandate_id, charge.detail);
        }
        break;

    case ARW_CHARGE_ERROR:
    default:
        /* Une panne technique ne compte PAS comme un échec de paiement : la
         * base injoignable ou une monnaie désactivée ne sont pas la faute de
         * l'abonné, et les faire compter le pousserait vers la mise en demeure
         * pour une panne de notre côté. On réessaie au tick suivant. */
        stats->errored++;
        LOG_ERROR("mandat %s : %s", mandate_id, charge.detail);
        break;
    }
}

int arw_mandate_sweep_grace(PGconn *conn, const arw_config *cfg)
{
    /* Le passage en défaut est une écriture comme une autre : une passe de
     * simulation ne doit pas la produire. */
    if (cfg->dry_run) {
        return 0;
    }

    /*
     * Une seule instruction, donc une seule transaction implicite : le mandat
     * passe en DEFAULTED et le compte est sanctionné ensemble, ou pas du tout.
     * Un état où le mandat serait clos sans que l'abonnement soit repris —
     * l'abonné garderait son palier gratuitement, à vie — ne peut pas exister.
     *
     * La sanction est double, parce que l'invariant du mandat rend la première
     * moitié indispensable :
     *
     *   - `subscription_expires_at = NOW()` et le retour en `free` : tant que
     *     le mandat vivait, cette colonne valait NULL, et le balayage horaire
     *     de l'API filtre `AND subscription_expires_at IS NOT NULL`. Sans cette
     *     ligne, RIEN ne rétrograde jamais le compte. `premium` est écrit à la
     *     main : les hooks Sequelize ne voient pas une écriture SQL directe.
     *
     *   - `is_suspended = true` avec `suspended_until = NULL`, c'est-à-dire une
     *     suspension sans terme : `banMiddleware` ne relève que les suspensions
     *     dont l'échéance est passée. Elle ne se lève donc que par un Gardien.
     *     Aucune invalidation de cache à faire : `globalBanMiddleware` relit la
     *     base avec un TTL de 30 s, la bascule prend effet toute seule.
     *
     * `ban_count` n'est PAS incrémenté : il compte les violations des règles de
     * la communauté, et à 5 il vaut bannissement définitif. Un impayé n'a rien
     * à y faire.
     */
    static const char *sql =
        "WITH defaulted AS ( "
        "  UPDATE subscription_mandates "
        "     SET state = 'DEFAULTED', "
        "         next_retry_at = NULL, "
        "         updated_at = NOW() "
        "   WHERE state = 'GRACE' "
        "     AND grace_until IS NOT NULL "
        "     AND grace_until <= NOW() "
        "  RETURNING id, user_id, tier "
        "), sanctioned AS ( "
        "  UPDATE users u "
        "     SET subscription_tier       = 'free', "
        "         premium                 = false, "
        "         subscription_expires_at = NOW(), "
        "         is_suspended            = true, "
        "         suspended_at            = NOW(), "
        "         suspended_until         = NULL, "
        "         suspension_reason       = 'Abonnement non regularise : mandat de renouvellement en defaut de paiement', "
        "         suspension_meta         = jsonb_build_object( "
        "                                     'source', 'twitninf-autorenew', "
        "                                     'kind', 'mandate_defaulted', "
        "                                     'mandate_id', d.id::text, "
        "                                     'tier', d.tier::text, "
        "                                     'at', NOW()), "
        "         updated_at              = NOW() "
        "    FROM defaulted d "
        "   WHERE u.id = d.user_id "
        "  RETURNING u.id "
        ") "
        "SELECT id, user_id, tier FROM defaulted";

    PGresult *res = arw_db_query(conn, sql, 0, NULL);
    if (!res) {
        return -1;
    }

    int count = PQntuples(res);
    for (int i = 0; i < count; i++) {
        const char *mandate_id = PQgetvalue(res, i, 0);
        const char *user_id    = PQgetvalue(res, i, 1);
        const char *label      = tier_label(PQgetvalue(res, i, 2));

        char title[128];
        char message[512];
        char content[256];

        snprintf(title, sizeof(title), "Abonnement %s non regularise", label);
        snprintf(message, sizeof(message),
                 "Le delai de regularisation de votre abonnement %s est ecoule. "
                 "Votre abonnement prend fin et votre compte est suspendu. "
                 "Contactez le support pour regulariser votre situation.", label);
        snprintf(content, sizeof(content),
                 "{\"kind\":\"mandate_defaulted\",\"mandate_id\":\"%s\",\"suspended\":true}",
                 mandate_id);

        /* La notification est écrite APRÈS la suspension, et `/api/notifications`
         * fait partie des routes ouvertes aux comptes bloqués : l'abonné peut
         * encore lire pourquoi il est suspendu. */
        arw_notify(conn, user_id, title, message, "urgent", content);

        LOG_WARN("mandat %s (user %s) : DEFAULTED — abonnement repris, compte suspendu",
                 mandate_id, user_id);
    }

    PQclear(res);
    return count;
}
