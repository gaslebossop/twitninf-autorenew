#include "db.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Clé arbitraire mais stable du verrou consultatif. Changer cette valeur
 * revient à autoriser deux démons à tourner ensemble. */
#define ARW_SINGLETON_LOCK_KEY 0x7741524557UL /* "wAREW" */

PGconn *arw_db_connect(const char *uri)
{
    PGconn *conn = PQconnectdb(uri);
    if (PQstatus(conn) != CONNECTION_OK) {
        LOG_ERROR("connexion Postgres refusée : %s", PQerrorMessage(conn));
        PQfinish(conn);
        return NULL;
    }
    return conn;
}

bool arw_db_ensure(PGconn **conn, const char *uri)
{
    if (*conn && PQstatus(*conn) == CONNECTION_OK) {
        return true;
    }

    if (*conn) {
        LOG_WARN("connexion Postgres perdue — reconnexion");
        PQfinish(*conn);
        *conn = NULL;
    }

    *conn = arw_db_connect(uri);
    if (!*conn) {
        return false;
    }

    /* Le verrou consultatif vit dans la SESSION : une reconnexion le perd et
     * doit le reprendre, sinon un second démon pourrait s'emparer de la place
     * pendant la coupure et travailler en double. */
    if (!arw_db_try_singleton_lock(*conn)) {
        LOG_ERROR("une autre instance détient déjà le verrou de prélèvement");
        PQfinish(*conn);
        *conn = NULL;
        return false;
    }

    LOG_INFO("connecté à Postgres");
    return true;
}

PGresult *arw_db_query(PGconn *conn, const char *sql, int nparams,
                       const char *const *params)
{
    PGresult *res = PQexecParams(conn, sql, nparams, NULL, params, NULL, NULL, 0);
    ExecStatusType status = PQresultStatus(res);

    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        LOG_ERROR("SQL échoué (%s) : %s", PQresStatus(status), PQerrorMessage(conn));
        LOG_DEBUG("requête : %s", sql);
        PQclear(res);
        return NULL;
    }
    return res;
}

long arw_db_command(PGconn *conn, const char *sql, int nparams,
                    const char *const *params)
{
    PGresult *res = arw_db_query(conn, sql, nparams, params);
    if (!res) {
        return -1;
    }

    const char *touched = PQcmdTuples(res);
    long rows = (touched && *touched) ? strtol(touched, NULL, 10) : 0;
    PQclear(res);
    return rows;
}

bool arw_db_begin(PGconn *conn)
{
    return arw_db_command(conn, "BEGIN", 0, NULL) >= 0;
}

bool arw_db_commit(PGconn *conn)
{
    return arw_db_command(conn, "COMMIT", 0, NULL) >= 0;
}

void arw_db_rollback(PGconn *conn)
{
    PGresult *res = PQexec(conn, "ROLLBACK");
    PQclear(res);
}

bool arw_db_try_singleton_lock(PGconn *conn)
{
    char key[32];
    snprintf(key, sizeof(key), "%lu", (unsigned long)ARW_SINGLETON_LOCK_KEY);
    const char *params[1] = { key };

    PGresult *res = arw_db_query(conn, "SELECT pg_try_advisory_lock($1::bigint)",
                                 1, params);
    if (!res) {
        return false;
    }

    bool acquired = (PQntuples(res) == 1 && strcmp(PQgetvalue(res, 0, 0), "t") == 0);
    PQclear(res);
    return acquired;
}

static bool random_bytes(unsigned char *buf, size_t len)
{
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        LOG_ERROR("/dev/urandom illisible — aucun identifiant ne sera généré");
        return false;
    }

    size_t got = fread(buf, 1, len, urandom);
    fclose(urandom);

    if (got != len) {
        LOG_ERROR("/dev/urandom a rendu %zu octets sur %zu", got, len);
        return false;
    }
    return true;
}

static void hexify(const unsigned char *bytes, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

bool arw_db_uuid4(arw_uuid out)
{
    unsigned char raw[16];
    if (!random_bytes(raw, sizeof(raw))) {
        return false;
    }

    /* Version 4 et variante RFC 4122 : sans ces deux masques on produit un
     * identifiant aléatoire de 128 bits, pas un UUID v4 — l'API et l'admin
     * les relisent comme des UUID. */
    raw[6] = (unsigned char)((raw[6] & 0x0f) | 0x40);
    raw[8] = (unsigned char)((raw[8] & 0x3f) | 0x80);

    char hex[33];
    hexify(raw, sizeof(raw), hex);

    snprintf(out, ARW_UUID_LEN + 1, "%.8s-%.4s-%.4s-%.4s-%.12s",
             hex, hex + 8, hex + 12, hex + 16, hex + 20);
    return true;
}

bool arw_db_tx_hash(char out[ARW_HASH_LEN + 1])
{
    unsigned char raw[32];
    if (!random_bytes(raw, sizeof(raw))) {
        return false;
    }
    hexify(raw, sizeof(raw), out);
    return true;
}
