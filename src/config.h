#ifndef ARW_CONFIG_H
#define ARW_CONFIG_H

#include "common.h"

/*
 * Toute la configuration vient de l'environnement : le démon tourne sous
 * systemd avec un EnvironmentFile, exactement comme le reste de la pile VPS.
 * Aucun secret n'est écrit en dur ici.
 */
typedef struct {
    char pguri[1024];          /* ARW_PGURI — chaîne de connexion libpq */
    char treasury_user_id[ARW_UUID_LEN + 1];

    int tick_seconds;          /* sommeil entre deux passes */
    int batch_size;            /* mandats traités par passe */
    int retry_hours;           /* cadence de réessai en impayé */
    int max_failures;          /* échecs avant mise en demeure */
    int grace_days;            /* délai de régularisation accordé */
    int period_days;           /* durée d'une période d'abonnement */
    int tweet_credits;         /* crédits rechargés à chaque paiement confirmé */

    double price_eur_plus;     /* tarifés en euros, reconvertis au cours */
    double price_eur_pro;
    double price_nf_ultra;     /* tarifé en NF fixe — pas de conversion */

    bool dry_run;              /* journalise les prélèvements sans les écrire */
    bool once;                 /* une seule passe, puis sortie (essais, cron) */
    bool verbose;
} arw_config;

/* Renvoie false et journalise si une valeur obligatoire manque ou est absurde. */
bool arw_config_load(arw_config *cfg);

/* Prix d'une période pour un palier, en NF, au cours donné.
 * Renvoie -1 si le palier est inconnu ou le cours inexploitable. */
double arw_config_price_nf(const arw_config *cfg, const char *tier, double nf_price_eur);

#endif /* ARW_CONFIG_H */
