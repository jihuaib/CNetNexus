#ifndef SRV6_DB_H
#define SRV6_DB_H

#include <glib.h>
#include <stdint.h>

#include "net_addr.h"
#include "srv6.h"

#define SRV6_TABLE_LOCATOR "srv6_locator"
#define SRV6_TABLE_SID "srv6_sid"

#define SRV6_DEFAULT_FUNCTION_BITS 16u
#define SRV6_FUNCTION_BITS_MAX 32u

typedef struct srv6_locator
{
    char name[SRV6_LOCATOR_NAME_MAX];
    net_addr_t prefix;
    uint8_t prefix_len;
    uint8_t function_bits;
    uint8_t _pad0[2];
} srv6_locator_t;

int srv6_db_init(void);
int srv6_db_restore(void);

int srv6_db_locator_upsert(const srv6_locator_t *locator);
int srv6_db_locator_delete(const char *name);
int srv6_db_locator_list(GPtrArray **out);

int srv6_db_sid_insert(const srv6_sid_entry_t *entry);
int srv6_db_sid_delete(const srv6_sid_key_t *key);
int srv6_db_sid_list(GPtrArray **out);

int srv6_db_delete_config(void);

void srv6_db_sid_key_text(const srv6_sid_key_t *key, char *buf, size_t buf_len);

#endif /* SRV6_DB_H */
