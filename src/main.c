/*
 * twitninf-autorenew — renouvellement automatique des abonnements TwitNinf,
 * prélevé sur les portefeuilles NF.
 *
 * Le démon exécute un mandat signé par l'utilisateur. L'autorisation
 * anti-fraude est prise UNE FOIS, à la signature, par l'API Node ; les
 * prélèvements suivants ne repassent pas par le moteur de risque. Le seul
 * contrôle conservé à chaque cycle est `user_wallets.is_locked`, qui reste le
 * bouton d'arrêt d'un Gardien.
 *
 * Tant qu'un mandat vit, `users.subscription_expires_at` vaut NULL :
 * l'abonnement n'expire pas, y compris pendant un impayé.
 */

#include "charge.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include "mandate.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

/*
 * Sommeil découpé en tranches d'une seconde : un SIGTERM pendant l'attente
 * doit rendre la main tout de suite. Sans ce découpage, systemd attendrait la
 * fin du tick avant de pouvoir arrêter le service.
 */
static void interruptible_sleep(int seconds)
{
    for (int i = 0; i < seconds && !g_stop; i++) {
        struct timespec one_second = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&one_second, NULL);
    }
}

static void install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Une écriture sur un socket Postgres refermé ne doit pas tuer le
     * processus : libpq rapporte l'erreur, on reconnecte au tick suivant. */
    signal(SIGPIPE, SIG_IGN);
}

static void run_pass(PGconn *conn, const arw_config *cfg, arw_uuid *ids)
{
    int defaulted = arw_mandate_sweep_grace(conn, cfg);
    if (defaulted > 0) {
        LOG_WARN("%d mandat(s) en defaut de paiement — abonnement repris, compte suspendu",
                 defaulted);
    }

    int due = arw_mandate_collect_due(conn, cfg, ids, cfg->batch_size);
    if (due < 0) {
        LOG_ERROR("selection des mandats impossible — passe abandonnee");
        return;
    }
    if (due == 0) {
        LOG_DEBUG("aucun mandat du");
        return;
    }

    arw_pass_stats stats;
    memset(&stats, 0, sizeof(stats));

    for (int i = 0; i < due && !g_stop; i++) {
        arw_mandate_process(conn, cfg, ids[i], &stats);
    }

    LOG_INFO("passe terminee : %d preleve(s), %d impaye(s), %d ignore(s), %d erreur(s)",
             stats.charged, stats.unpaid, stats.skipped, stats.errored);
}

int main(void)
{
    install_signal_handlers();

    arw_config cfg;
    if (!arw_config_load(&cfg)) {
        return 1;
    }

    /* Le lot est alloué une fois : la boucle ne fait aucune allocation par
     * passe, et un pic de mandats dus ne peut pas faire échouer un malloc au
     * milieu d'un cycle de prélèvements. */
    arw_uuid *ids = calloc((size_t)cfg.batch_size, sizeof(arw_uuid));
    if (!ids) {
        LOG_ERROR("allocation du lot de %d mandats impossible", cfg.batch_size);
        return 1;
    }

    PGconn *conn = NULL;
    LOG_INFO("twitninf-autorenew demarre");

    int exit_code = 0;

    while (!g_stop) {
        if (!arw_db_ensure(&conn, cfg.pguri)) {
            /* Base injoignable, ou verrou consultatif détenu par une autre
             * instance : on repasse au tick suivant sans rien compter comme
             * un échec de paiement. */
            if (cfg.once) {
                exit_code = 1;
                break;
            }
            interruptible_sleep(cfg.tick_seconds);
            continue;
        }

        run_pass(conn, &cfg, ids);

        /* ARW_ONCE : une passe et on sort. C'est le mode des essais, et celui
         * d'un déclenchement par cron si on préfère ça au service permanent. */
        if (cfg.once) {
            break;
        }
        interruptible_sleep(cfg.tick_seconds);
    }

    if (!cfg.once) {
        LOG_INFO("arret demande - fermeture");
    }
    if (conn) {
        PQfinish(conn);
    }
    free(ids);
    return exit_code;
}
