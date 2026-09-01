#include "config.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Trésorerie de la plateforme — même valeur par défaut que economy/constants.js.
 * Surchargeable, parce que la base de recette n'a pas le même compte. */
#define ARW_DEFAULT_TREASURY "01837802-c2ae-4de0-9471-7a9b642afde2"

static const char *env_str(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && *value) ? value : fallback;
}

static int env_int(const char *name, int fallback, int min, int max)
{
    const char *raw = getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }

    char *end = NULL;
    long parsed = strtol(raw, &end, 10);
    if (end == raw || *end != '\0' || parsed < min || parsed > max) {
        LOG_WARN("%s = \"%s\" est hors bornes [%d, %d] — valeur par défaut %d retenue",
                 name, raw, min, max, fallback);
        return fallback;
    }
    return (int)parsed;
}

static double env_double(const char *name, double fallback, double min, double max)
{
    const char *raw = getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }

    char *end = NULL;
    double parsed = strtod(raw, &end);
    if (end == raw || *end != '\0' || !(parsed >= min) || !(parsed <= max)) {
        LOG_WARN("%s = \"%s\" est hors bornes [%g, %g] — valeur par défaut %g retenue",
                 name, raw, min, max, fallback);
        return fallback;
    }
    return parsed;
}

static bool env_bool(const char *name, bool fallback)
{
    const char *raw = getenv(name);
    if (!raw || !*raw) {
        return fallback;
    }
    return (strcmp(raw, "1") == 0 || strcasecmp(raw, "true") == 0 ||
            strcasecmp(raw, "yes") == 0 || strcasecmp(raw, "on") == 0);
}

static bool copy_bounded(char *dst, size_t cap, const char *src, const char *name)
{
    size_t len = strlen(src);
    if (len == 0) {
        LOG_ERROR("%s est vide", name);
        return false;
    }
    if (len >= cap) {
        LOG_ERROR("%s dépasse %zu caractères", name, cap - 1);
        return false;
    }
    memcpy(dst, src, len + 1);
    return true;
}

bool arw_config_load(arw_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const char *pguri = getenv("ARW_PGURI");
    if (!pguri || !*pguri) {
        LOG_ERROR("ARW_PGURI est obligatoire (chaîne de connexion libpq)");
        return false;
    }
    if (!copy_bounded(cfg->pguri, sizeof(cfg->pguri), pguri, "ARW_PGURI")) {
        return false;
    }

    if (!copy_bounded(cfg->treasury_user_id, sizeof(cfg->treasury_user_id),
                      env_str("ARW_TREASURY_USER_ID", ARW_DEFAULT_TREASURY),
                      "ARW_TREASURY_USER_ID")) {
        return false;
    }

    /* Le tick est court et la cadence de réessai est portée par next_retry_at,
     * pas par le sommeil du process : dormir trois heures ferait attendre
     * jusqu'à trois heures une échéance parfaitement saine. */
    cfg->tick_seconds = env_int("ARW_TICK_SECONDS", 60, 5, 3600);
    cfg->batch_size   = env_int("ARW_BATCH_SIZE", 200, 1, 10000);
    cfg->retry_hours  = env_int("ARW_RETRY_HOURS", 3, 1, 168);
    cfg->max_failures = env_int("ARW_MAX_FAILURES", 20, 1, 1000);
    cfg->grace_days   = env_int("ARW_GRACE_DAYS", 30, 1, 365);
    cfg->period_days  = env_int("ARW_PERIOD_DAYS", 5, 1, 365);
    cfg->tweet_credits = env_int("ARW_TWEET_CREDITS", 5, 0, 1000);

    cfg->price_eur_plus = env_double("ARW_PRICE_EUR_PLUS", 15.0, 0.01, 100000.0);
    cfg->price_eur_pro  = env_double("ARW_PRICE_EUR_PRO", 30.0, 0.01, 100000.0);
    cfg->price_nf_ultra = env_double("ARW_PRICE_NF_ULTRA", 300.0, 0.01, 1000000.0);

    cfg->dry_run = env_bool("ARW_DRY_RUN", false);
    cfg->once    = env_bool("ARW_ONCE", false);
    cfg->verbose = env_bool("ARW_VERBOSE", false);

    if (cfg->verbose) {
        arw_log_set_level(ARW_LOG_DEBUG);
    }

    LOG_INFO("configuration : tick=%ds lot=%d réessai=%dh seuil=%d grâce=%dj période=%dj%s",
             cfg->tick_seconds, cfg->batch_size, cfg->retry_hours,
             cfg->max_failures, cfg->grace_days, cfg->period_days,
             cfg->dry_run ? " [SIMULATION]" : "");

    return true;
}

/*
 * Reproduit exactement la tarification de l'API.
 *
 * Plus et Pro sont tarifés en EUROS (constants/subscriptionTiers.js) et
 * reconvertis au cours du moment : 15 € restent 15 € quand le NF bouge, seul
 * le nombre de NF change. `nfAmountForEur` arrondit au dix-millième, puis
 * `assertPositive` → `roundTWC` ramène à deux décimales le montant réellement
 * débité. Les deux arrondis sont conservés dans cet ordre, sinon le montant
 * prélevé par le démon diffère d'un centime de NF de celui prélevé par l'API
 * pour le même palier au même cours.
 *
 * Ultra est tarifé en NF FIXE (300 NF) : aucune conversion, aucune dépendance
 * au cours.
 */
double arw_config_price_nf(const arw_config *cfg, const char *tier, double nf_price_eur)
{
    if (strcmp(tier, "ultra") == 0) {
        double round2 = (double)(long long)(cfg->price_nf_ultra * 100.0 + 0.5) / 100.0;
        return round2 > 0.0 ? round2 : -1.0;
    }

    double eur;
    if (strcmp(tier, "plus") == 0) {
        eur = cfg->price_eur_plus;
    } else if (strcmp(tier, "pro") == 0) {
        eur = cfg->price_eur_pro;
    } else {
        return -1.0;
    }

    if (!(nf_price_eur > 0.0)) {
        return -1.0;
    }

    double raw = eur / nf_price_eur;
    double round4 = (double)(long long)(raw * 10000.0 + 0.5) / 10000.0;
    double round2 = (double)(long long)(round4 * 100.0 + 0.5) / 100.0;
    return round2 > 0.0 ? round2 : -1.0;
}
