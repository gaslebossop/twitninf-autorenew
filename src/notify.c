#include "notify.h"
#include "db.h"
#include "log.h"

#include <string.h>

bool arw_notify(PGconn *conn, const char *user_id, const char *title,
                const char *message, const char *priority, const char *content_json)
{
    arw_uuid id;
    if (!arw_db_uuid4(id)) {
        return false;
    }

    /* Les types des paramètres sont déduits des colonnes cibles : l'ENUM
     * `type` et les JSONB `content`/`metadata` sont donc convertis par le
     * serveur sans cast explicite. */
    static const char *sql =
        "INSERT INTO notifications "
        "  (id, recipient_id, type, title, message, content, is_read, priority, "
        "   metadata, created_at, updated_at) "
        "VALUES ($1, $2, 'premium', $3, $4, $5, false, $6, "
        "        '{\"source\":\"autorenew\"}', NOW(), NOW())";

    const char *params[6] = {
        id,
        user_id,
        title,
        message,
        content_json ? content_json : "{}",
        priority ? priority : "normal"
    };

    if (arw_db_command(conn, sql, 6, params) < 0) {
        LOG_ERROR("notification non enregistrée pour %s : %s", user_id, title);
        return false;
    }
    return true;
}
