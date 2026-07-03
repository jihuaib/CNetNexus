/**
 * @file   snmp.h
 * @brief  SNMP module public report protocol
 */
#ifndef NETNEXUS_SNMP_H
#define NETNEXUS_SNMP_H

#include <stdint.h>

#include "dev.h"

#define SNMP_OID_MAX_LEN 128
#define SNMP_VALUE_MAX_LEN 256
#define SNMP_TRAP_VAR_MAX 8
#define SNMP_COMMUNITY_MAX_LEN 64
#define SNMP_TRAP_HOST_MAX_LEN 128

#define SNMP_TRAP_DEFAULT_PORT 162

#define SNMP_MSG_TYPE_VALUE_SET DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SNMP, 0x0001)
#define SNMP_MSG_TYPE_SUBTREE_CLEAR DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SNMP, 0x0002)
#define SNMP_MSG_TYPE_TRAP_SEND DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SNMP, 0x0003)
#define SNMP_MSG_TYPE_CONFIG_SET DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_SNMP, 0x0004)

typedef enum snmp_value_type
{
    SNMP_VALUE_INTEGER = 1,
    SNMP_VALUE_GAUGE = 2,
    SNMP_VALUE_TIMETICKS = 3,
    SNMP_VALUE_STRING = 4,
    SNMP_VALUE_OID = 5,
    SNMP_VALUE_OCTETS = 6,
    SNMP_VALUE_IPADDR = 7
} snmp_value_type_t;

typedef struct snmp_value_msg
{
    uint32_t owner_module_id;
    uint32_t value_type;
    char oid[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];
} snmp_value_msg_t;

typedef struct snmp_subtree_clear_msg
{
    uint32_t owner_module_id;
    char oid_prefix[SNMP_OID_MAX_LEN];
} snmp_subtree_clear_msg_t;

typedef struct snmp_trap_var
{
    uint32_t value_type;
    char oid[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];
} snmp_trap_var_t;

typedef struct snmp_trap_msg
{
    uint32_t owner_module_id;
    char trap_oid[SNMP_OID_MAX_LEN];
    uint32_t var_count;
    snmp_trap_var_t vars[SNMP_TRAP_VAR_MAX];
} snmp_trap_msg_t;

typedef struct snmp_config_msg
{
    uint32_t trap_enabled;
    char trap_host[SNMP_TRAP_HOST_MAX_LEN];
    uint32_t trap_port;
} snmp_config_msg_t;

#endif /* NETNEXUS_SNMP_H */
