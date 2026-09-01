#ifndef ARW_NOTIFY_H
#define ARW_NOTIFY_H

#include "common.h"

#include <libpq-fe.h>

/*
 * Insère une notification dans la table `notifications` de l'API.
 *
 * Le type utilisé est 'premium', qui existe déjà dans l'ENUM du modèle : aucune
 * migration d'ENUM à jouer, et l'app mobile sait déjà router ce type vers
 * l'écran d'abonnement.
 *
 * `content` doit être un objet JSON valide (ou NULL pour "{}").
 * `priority` : "low" | "normal" | "high" | "urgent".
 */
bool arw_notify(PGconn *conn, const char *user_id, const char *title,
                const char *message, const char *priority, const char *content_json);

#endif /* ARW_NOTIFY_H */
