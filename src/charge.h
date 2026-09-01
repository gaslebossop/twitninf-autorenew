#ifndef ARW_CHARGE_H
#define ARW_CHARGE_H

#include "common.h"
#include "config.h"

#include <libpq-fe.h>

typedef enum {
    ARW_CHARGE_OK,            /* prélevé, abonnement reconduit */
    ARW_CHARGE_INSUFFICIENT,  /* solde NF trop bas — impayé */
    ARW_CHARGE_WALLET_LOCKED, /* portefeuille gelé par un Gardien — impayé */
    ARW_CHARGE_SKIPPED,       /* mandat plus dû, annulé, ou pris par une autre passe */
    ARW_CHARGE_ERROR          /* panne technique — ni débit, ni comptage d'échec */
} arw_charge_status;

typedef struct {
    arw_charge_status status;
    arw_uuid user_id;
    char     tier[8];
    arw_uuid tx_id;
    char     amount_nf[32];   /* montant exact, tel qu'écrit en base */
    char     balance[32];     /* solde constaté avant prélèvement */
    char     detail[256];
} arw_charge_result;

/*
 * Exécute un prélèvement complet dans UNE transaction Postgres : verrous
 * portefeuilles, débit, crédit trésorerie, ligne de grand livre, reconduction
 * de l'abonnement, avancement du mandat. Tout ou rien.
 *
 * Ne repasse pas par le moteur anti-fraude : l'autorisation est prise une seule
 * fois, à la signature du mandat, par l'API. Le seul contrôle conservé ici est
 * `user_wallets.is_locked` — une lecture d'une colonne, et le seul bouton qui
 * permette d'arrêter un mandat qui s'emballe.
 */
arw_charge_result arw_charge_run(PGconn *conn, const arw_config *cfg,
                                 const char *mandate_id);

#endif /* ARW_CHARGE_H */
