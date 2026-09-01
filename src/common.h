#ifndef ARW_COMMON_H
#define ARW_COMMON_H

#include <stdbool.h>
#include <stddef.h>

/* 36 caractères canoniques : 8-4-4-4-12 */
#define ARW_UUID_LEN 36
/* 32 octets aléatoires en hexadécimal — largeur de transactions.transaction_hash */
#define ARW_HASH_LEN 64

typedef char arw_uuid[ARW_UUID_LEN + 1];

#endif /* ARW_COMMON_H */
