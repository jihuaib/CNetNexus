/**
 * @file   bgp_snmp_report.c
 * @brief  BGP SNMP standard notification reporting
 */
#include "bgp_snmp_report.h"

#include <arpa/inet.h>
#include <string.h>

#include "bgp_main.h"
#include "log.h"
#include "net_addr.h"
#include "snmp.h"

#define BGP4_MIB_TRAP_ESTABLISHED ".1.3.6.1.2.1.15.0.1"
#define BGP4_MIB_TRAP_BACKWARD_TRANS ".1.3.6.1.2.1.15.0.2"
#define BGP4_MIB_PEER_REMOTE_ADDR ".1.3.6.1.2.1.15.3.1.7"
#define BGP4_MIB_PEER_LAST_ERROR ".1.3.6.1.2.1.15.3.1.14"
#define BGP4_MIB_PEER_STATE ".1.3.6.1.2.1.15.3.1.2"

static int bgp_snmp_peer_ipv4_index(const net_addr_t *addr, char *idx, size_t idx_sz, char *addr_str, size_t addr_sz)
{
    if (!addr || addr->family != AF_INET || !idx || idx_sz == 0 || !addr_str || addr_sz == 0)
    {
        return -1;
    }

    const unsigned char *octets = (const unsigned char *)&addr->u.v4.s_addr;
    snprintf(idx, idx_sz, "%u.%u.%u.%u", (unsigned)octets[0], (unsigned)octets[1], (unsigned)octets[2],
             (unsigned)octets[3]);
    net_addr_to_str(addr, addr_str, addr_sz);
    return 0;
}

static uint32_t bgp_snmp_state_value(bgp_fsm_state_t state)
{
    if ((unsigned)state >= BGP_FSM_STATE_MAX)
    {
        return 1u;
    }
    return (uint32_t)state + 1u;
}

void bgp_snmp_report_neighbor_state(const bgp_session_t *sess, bgp_fsm_state_t old_state, bgp_fsm_state_t new_state)
{
    if (!sess)
    {
        return;
    }

    const gboolean was_established = old_state == BGP_FSM_STATE_ESTABLISHED;
    const gboolean is_established = new_state == BGP_FSM_STATE_ESTABLISHED;
    if (was_established == is_established)
    {
        return;
    }

    dev_ipc_context_t *ctx = bgp_local_ipc_ctx();
    if (!ctx || !dev_ipc_is_connected(ctx, DEV_MODULE_ID_SNMP))
    {
        return;
    }

    char peer_index[32] = {0};
    char peer_addr[64] = {0};
    if (bgp_snmp_peer_ipv4_index(&sess->neighbor_addr, peer_index, sizeof(peer_index), peer_addr, sizeof(peer_addr)) !=
        0)
    {
        LOG_DEBUG("BGP: skip SNMP BGP4-MIB trap for non-IPv4 neighbor");
        return;
    }

    snmp_trap_msg_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.owner_module_id = DEV_MODULE_ID_BGP;
    g_strlcpy(payload.trap_oid, is_established ? BGP4_MIB_TRAP_ESTABLISHED : BGP4_MIB_TRAP_BACKWARD_TRANS,
              sizeof(payload.trap_oid));
    payload.var_count = 3;

    snprintf(payload.vars[0].oid, sizeof(payload.vars[0].oid), "%s.%s", BGP4_MIB_PEER_REMOTE_ADDR, peer_index);
    payload.vars[0].value_type = SNMP_VALUE_IPADDR;
    g_strlcpy(payload.vars[0].value, peer_addr, sizeof(payload.vars[0].value));

    snprintf(payload.vars[1].oid, sizeof(payload.vars[1].oid), "%s.%s", BGP4_MIB_PEER_LAST_ERROR, peer_index);
    payload.vars[1].value_type = SNMP_VALUE_OCTETS;
    g_strlcpy(payload.vars[1].value, "00 00", sizeof(payload.vars[1].value));

    snprintf(payload.vars[2].oid, sizeof(payload.vars[2].oid), "%s.%s", BGP4_MIB_PEER_STATE, peer_index);
    payload.vars[2].value_type = SNMP_VALUE_INTEGER;
    snprintf(payload.vars[2].value, sizeof(payload.vars[2].value), "%u", bgp_snmp_state_value(new_state));

    snmp_trap_msg_t *dup = (snmp_trap_msg_t *)g_memdup2(&payload, sizeof(payload));
    if (!dup)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(SNMP_MSG_TYPE_TRAP_SEND, DEV_MODULE_ID_BGP, DEV_MODULE_ID_SNMP, 0,
                                                    dup, sizeof(*dup), g_free);
    if (!msg)
    {
        g_free(dup);
        return;
    }

    if (dev_ipc_send(ctx, DEV_MODULE_ID_SNMP, msg) != 0)
    {
        LOG_DEBUG("BGP: skip SNMP trap report neighbor=%s", peer_addr);
    }
    dev_ipc_message_free(msg);
}
