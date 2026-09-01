#ifndef ARW_MANDATE_H
#define ARW_MANDATE_H

#include "common.h"
#include "config.h"

#include <libpq-fe.h>

typedef struct {
    int charged;      /* prélèvements réussis */
    int unpaid;       /* solde insuffisant ou portefeuille gelé */
    int skipped;      /* plus dus, annulés, ou simulés */
    int errored;      /* pannes techniques — aucun comptage d'échec */
    int defaulted;    /* délai de régularisation épuisé */
} arw_pass_stats;

/*
 * Relève les mandats à traiter : échéances atteintes pour les mandats sains,
 * réessais arrivés à terme pour les mandats en impayé. Sans verrou — c'est le
 * prélèvement lui-même qui verrouille et revérifie l'échéance.
 *
 * Renvoie le nombre d'identifiants écrits dans `ids`, ou -1 en cas d'échec.
 */
int arw_mandate_collect_due(PGconn *conn, const arw_config *cfg,
                            arw_uuid *ids, int max_ids);

/* Traite un mandat : prélèvement, puis suite donnée au résultat. */
void arw_mandate_process(PGconn *conn, const arw_config *cfg,
                         const char *mandate_id, arw_pass_stats *stats);

/*
 * Passe les mandats dont le délai de régularisation est épuisé en DEFAULTED,
 * sanctionne les comptes concernés et les prévient.
 *
 * La sanction est prononcée par le démon, dans la MÊME instruction que le
 * passage en défaut : retour en `free` avec `subscription_expires_at = NOW()`
 * — sans quoi l'invariant du mandat (NULL = pas d'expiration) laisserait le
 * palier acquis à vie — et `is_suspended = true` sans terme, donc levable
 * seulement par un Gardien. `ban_count` reste intact : un impayé n'est pas une
 * violation des règles de la communauté.
 */
int arw_mandate_sweep_grace(PGconn *conn, const arw_config *cfg);

#endif /* ARW_MANDATE_H */
