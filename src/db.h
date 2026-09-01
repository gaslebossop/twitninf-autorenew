#ifndef ARW_DB_H
#define ARW_DB_H

#include "common.h"

#include <libpq-fe.h>

/* Ouvre une connexion, ou NULL. */
PGconn *arw_db_connect(const char *uri);

/* Garantit une connexion utilisable : reconnecte si elle est tombée.
 * Renvoie false si la reconnexion échoue — l'appelant réessaiera au tick suivant. */
bool arw_db_ensure(PGconn **conn, const char *uri);

/* Requête paramétrée. Le résultat doit être libéré par PQclear.
 * Renvoie NULL et journalise si la commande échoue. */
PGresult *arw_db_query(PGconn *conn, const char *sql, int nparams,
                       const char *const *params);

/* Commande sans résultat exploitable. Renvoie le nombre de lignes touchées,
 * ou -1 en cas d'échec. */
long arw_db_command(PGconn *conn, const char *sql, int nparams,
                    const char *const *params);

bool arw_db_begin(PGconn *conn);
bool arw_db_commit(PGconn *conn);
void arw_db_rollback(PGconn *conn); /* jamais fatal : on est déjà en train d'abandonner */

/*
 * Verrou consultatif de session : garantit qu'une seule instance du démon
 * prélève. Deux instances qui tournent en parallèle ne double-débiteraient pas
 * (chaque mandat est verrouillé par SELECT ... FOR UPDATE), mais elles se
 * bloqueraient mutuellement pour rien.
 */
bool arw_db_try_singleton_lock(PGconn *conn);

/* UUID v4 depuis /dev/urandom. False si l'entropie est indisponible — on
 * n'invente PAS d'identifiant de transaction à partir de rand(). */
bool arw_db_uuid4(arw_uuid out);

/* 32 octets aléatoires en hexadécimal, format de transactions.transaction_hash. */
bool arw_db_tx_hash(char out[ARW_HASH_LEN + 1]);

#endif /* ARW_DB_H */
