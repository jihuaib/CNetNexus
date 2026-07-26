/**
 * @file   lldp_snmp_report.c
 * @brief  LLDP-MIB SNMP walk data reporting
 */
#include "lldp_snmp_report.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "if.h"
#include "lldp_main.h"
#include "lldp_packet.h"
#include "lldp_worker.h"
#include "log.h"
#include "snmp.h"

#define LLDP_MIB_ROOT ".1.0.8802.1.1.2"
#define LLDP_LOC_CHASSIS_ID_SUBTYPE LLDP_MIB_ROOT ".1.3.1.0"
#define LLDP_LOC_CHASSIS_ID LLDP_MIB_ROOT ".1.3.2.0"
#define LLDP_LOC_SYS_NAME LLDP_MIB_ROOT ".1.3.3.0"
#define LLDP_LOC_SYS_DESC LLDP_MIB_ROOT ".1.3.4.0"
#define LLDP_LOC_SYS_CAP_SUPPORTED LLDP_MIB_ROOT ".1.3.5.0"
#define LLDP_LOC_SYS_CAP_ENABLED LLDP_MIB_ROOT ".1.3.6.0"
#define LLDP_LOC_PORT_PREFIX LLDP_MIB_ROOT ".1.3.7.1"
#define LLDP_REM_PREFIX LLDP_MIB_ROOT ".1.4.1.1"

static int lldp_snmp_connected(dev_ipc_context_t **ctx_out)
{
    dev_ipc_context_t *ctx = lldp_local_ipc_ctx();
    if (!ctx || !dev_ipc_is_connected(ctx, DEV_MODULE_ID_SNMP))
    {
        return 0;
    }
    if (ctx_out)
    {
        *ctx_out = ctx;
    }
    return 1;
}

static void lldp_snmp_send_value(dev_ipc_context_t *ctx, const char *oid, snmp_value_type_t type, const char *value)
{
    if (!ctx || !oid || !value)
    {
        return;
    }

    snmp_value_msg_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_module_id = DEV_MODULE_ID_LLDP;
    payload.value_type = (uint32_t)type;
    g_strlcpy(payload.oid, oid, sizeof(payload.oid));
    g_strlcpy(payload.value, value, sizeof(payload.value));

    snmp_value_msg_t *dup = (snmp_value_msg_t *)g_memdup2(&payload, sizeof(payload));
    if (!dup)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(SNMP_MSG_TYPE_VALUE_SET, DEV_MODULE_ID_LLDP, DEV_MODULE_ID_SNMP, 0,
                                                    dup, sizeof(*dup), g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }

    if (dev_ipc_send(ctx, DEV_MODULE_ID_SNMP, msg) != 0)
    {
        LOG_DEBUG("LLDP: skip SNMP value report oid=%s", oid);
    }
    dev_ipc_message_free(msg);
}

static void lldp_snmp_clear_tree(dev_ipc_context_t *ctx)
{
    if (!ctx)
    {
        return;
    }

    snmp_subtree_clear_msg_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_module_id = DEV_MODULE_ID_LLDP;
    g_strlcpy(payload.oid_prefix, LLDP_MIB_ROOT, sizeof(payload.oid_prefix));

    snmp_subtree_clear_msg_t *dup = (snmp_subtree_clear_msg_t *)g_memdup2(&payload, sizeof(payload));
    if (!dup)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(SNMP_MSG_TYPE_SUBTREE_CLEAR, DEV_MODULE_ID_LLDP, DEV_MODULE_ID_SNMP,
                                                    0, dup, sizeof(*dup), g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }

    if (dev_ipc_send(ctx, DEV_MODULE_ID_SNMP, msg) != 0)
    {
        LOG_DEBUG("LLDP: skip SNMP subtree clear");
    }
    dev_ipc_message_free(msg);
}

static uint32_t lldp_snmp_port_num(const char *ifname)
{
    if (!ifname || ifname[0] == '\0')
    {
        return 10000u;
    }

    const if_api_cache_entry_t *entry = if_api_cache_lookup(ifname);
    if (entry && entry->ifindex != 0u)
    {
        return entry->ifindex;
    }

    if (g_lldp_work_local && g_lldp_work_local->interfaces)
    {
        lldp_iface_state_t *iface = g_hash_table_lookup(g_lldp_work_local->interfaces, ifname);
        if (iface && iface->ifindex != 0u)
        {
            return iface->ifindex;
        }
    }

    return 10000u + (g_str_hash(ifname) % 50000u);
}

static void lldp_snmp_hostname(char *buf, size_t len)
{
    if (!buf || len == 0)
    {
        return;
    }
    if (gethostname(buf, len - 1u) != 0 || buf[0] == '\0')
    {
        g_strlcpy(buf, "netnexus", len);
    }
    buf[len - 1u] = '\0';
}

static void lldp_snmp_id_text(const uint8_t *data, uint16_t data_len, char *buf, size_t len)
{
    if (!buf || len == 0)
    {
        return;
    }
    buf[0] = '\0';
    if (!data || data_len == 0u)
    {
        return;
    }

    int printable = 1;
    for (uint16_t i = 0; i < data_len; ++i)
    {
        if (!isprint((unsigned char)data[i]))
        {
            printable = 0;
            break;
        }
    }
    if (printable)
    {
        size_t copy = data_len < len - 1u ? data_len : len - 1u;
        memcpy(buf, data, copy);
        buf[copy] = '\0';
        return;
    }

    size_t used = 0u;
    for (uint16_t i = 0; i < data_len && used + 3u < len; ++i)
    {
        int n = snprintf(buf + used, len - used, "%s%02x", i == 0u ? "" : ":", data[i]);
        if (n < 0 || (size_t)n >= len - used)
        {
            buf[len - 1u] = '\0';
            return;
        }
        used += (size_t)n;
    }
}

static void lldp_snmp_report_local_system(dev_ipc_context_t *ctx)
{
    char hostname[SNMP_VALUE_MAX_LEN] = {0};
    lldp_snmp_hostname(hostname, sizeof(hostname));

    lldp_snmp_send_value(ctx, LLDP_LOC_CHASSIS_ID_SUBTYPE, SNMP_VALUE_INTEGER,
                         "7"); /* local(7), using hostname as chassis ID */
    lldp_snmp_send_value(ctx, LLDP_LOC_CHASSIS_ID, SNMP_VALUE_STRING, hostname);
    lldp_snmp_send_value(ctx, LLDP_LOC_SYS_NAME, SNMP_VALUE_STRING, hostname);
    lldp_snmp_send_value(ctx, LLDP_LOC_SYS_DESC, SNMP_VALUE_STRING, "CNetNexus LLDP");
    lldp_snmp_send_value(ctx, LLDP_LOC_SYS_CAP_SUPPORTED, SNMP_VALUE_OCTETS, "00 10");
    lldp_snmp_send_value(ctx, LLDP_LOC_SYS_CAP_ENABLED, SNMP_VALUE_OCTETS, "00 10");
}

static void lldp_snmp_report_local_port(dev_ipc_context_t *ctx, const lldp_iface_state_t *iface)
{
    if (!ctx || !iface || iface->ifname[0] == '\0')
    {
        return;
    }

    uint32_t port_num = lldp_snmp_port_num(iface->ifname);
    const char *port_desc = iface->port_desc[0] ? iface->port_desc : iface->ifname;
    char oid[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];

    snprintf(value, sizeof(value), "%u", port_num);
    snprintf(oid, sizeof(oid), LLDP_LOC_PORT_PREFIX ".1.%u", port_num);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), LLDP_LOC_PORT_PREFIX ".2.%u", port_num);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, "5"); /* interfaceName(5) */

    snprintf(oid, sizeof(oid), LLDP_LOC_PORT_PREFIX ".3.%u", port_num);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, iface->ifname);

    snprintf(oid, sizeof(oid), LLDP_LOC_PORT_PREFIX ".4.%u", port_num);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, port_desc);
}

static void lldp_snmp_report_remote(dev_ipc_context_t *ctx, const char *key, const lldp_neighbor_t *n)
{
    if (!ctx || !n || n->ifname[0] == '\0')
    {
        return;
    }

    const uint32_t time_mark = 0u;
    const uint32_t port_num = lldp_snmp_port_num(n->ifname);
    const uint32_t rem_index = (g_str_hash(key && key[0] ? key : n->ifname) % 2147483000u) + 1u;
    char oid[SNMP_OID_MAX_LEN];
    char value[SNMP_VALUE_MAX_LEN];
    char index[64];
    char chassis[SNMP_VALUE_MAX_LEN] = {0};
    char port[SNMP_VALUE_MAX_LEN] = {0};

    snprintf(index, sizeof(index), "%u.%u.%u", time_mark, port_num, rem_index);
    lldp_snmp_id_text(n->chassis_id, n->chassis_len, chassis, sizeof(chassis));
    lldp_snmp_id_text(n->port_id, n->port_len, port, sizeof(port));

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".1.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_TIMETICKS, "0");

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".2.%s", index);
    snprintf(value, sizeof(value), "%u", port_num);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".3.%s", index);
    snprintf(value, sizeof(value), "%u", rem_index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".4.%s", index);
    snprintf(value, sizeof(value), "%u", (unsigned)n->chassis_subtype);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".5.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, chassis);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".6.%s", index);
    snprintf(value, sizeof(value), "%u", (unsigned)n->port_subtype);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_INTEGER, value);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".7.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, port);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".8.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, n->port_desc);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".9.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, n->system_name);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".10.%s", index);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_STRING, n->system_desc);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".11.%s", index);
    snprintf(value, sizeof(value), "%04x", (unsigned)n->caps_supported);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_OCTETS, value);

    snprintf(oid, sizeof(oid), LLDP_REM_PREFIX ".12.%s", index);
    snprintf(value, sizeof(value), "%04x", (unsigned)n->caps_enabled);
    lldp_snmp_send_value(ctx, oid, SNMP_VALUE_OCTETS, value);
}

void lldp_snmp_report_refresh(void)
{
    dev_ipc_context_t *ctx = NULL;
    if (!lldp_snmp_connected(&ctx))
    {
        return;
    }

    lldp_snmp_clear_tree(ctx);
    lldp_snmp_report_local_system(ctx);

    lldp_worker_lock();
    if (g_lldp_work_local && g_lldp_work_local->interfaces)
    {
        GHashTableIter it;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&it, g_lldp_work_local->interfaces);
        while (g_hash_table_iter_next(&it, &key, &val))
        {
            (void)key;
            lldp_snmp_report_local_port(ctx, (const lldp_iface_state_t *)val);
        }
    }
    if (g_lldp_work_local && g_lldp_work_local->neighbors)
    {
        GHashTableIter it;
        gpointer key = NULL;
        gpointer val = NULL;
        g_hash_table_iter_init(&it, g_lldp_work_local->neighbors);
        while (g_hash_table_iter_next(&it, &key, &val))
        {
            lldp_snmp_report_remote(ctx, (const char *)key, (const lldp_neighbor_t *)val);
        }
    }
    lldp_worker_unlock();
}
