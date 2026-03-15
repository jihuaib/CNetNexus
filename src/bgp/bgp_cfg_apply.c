/**
 * @file   bgp_cfg_apply.c
 * @brief  BGP 配置内存态应用实现（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/07
 */
#include "bgp_cfg_apply.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

#include "bgp_main.h"
#include "bgp_protocol.h"
#include "errcode.h"

uint32_t bgp_cfg_apply_protocol(gboolean is_no, uint32_t as_number)
{
    if (!g_bgp_local)
    {
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        bgp_listen_stop();
        bgp_protocol_destroy(g_bgp_local->protocol);
        g_bgp_local->protocol = NULL;
        return ERRCODE_SUCCESS;
    }

    if (g_bgp_local->protocol)
    {
        return ERRCODE_FAIL;
    }

    g_bgp_local->protocol = bgp_protocol_create(as_number);
    if (!g_bgp_local->protocol)
    {
        return ERRCODE_FAIL;
    }

    bgp_listen_start();
    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_instance(gboolean is_no, bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi)
{
    if (!vrf)
    {
        return ERRCODE_FAIL;
    }

    bgp_instance_t *inst = g_hash_table_lookup(vrf->inst_hash, bgp_inst_hash_key(afi, safi));

    if (is_no)
    {
        if (!inst || !inst->peer_hash)
        {
            return ERRCODE_SUCCESS;
        }

        GList *addr_strs = NULL;
        GHashTableIter iter;
        gpointer k, v;
        g_hash_table_iter_init(&iter, inst->peer_hash);
        while (g_hash_table_iter_next(&iter, &k, &v))
        {
            addr_strs = g_list_append(addr_strs, g_strdup((const char *)k));
        }

        for (GList *l = addr_strs; l; l = l->next)
        {
            net_addr_t addr;
            if (net_addr_from_str((const char *)l->data, &addr) == 0)
            {
                bgp_vrf_af_disable_neighbor(vrf, afi, safi, &addr);
                bgp_session_t *sess = bgp_vrf_find_session(vrf, &addr);
                if (sess && !bgp_vrf_neighbor_has_any_af(vrf, &addr))
                {
                    bgp_server_stop_session_conns(sess);
                }
            }
        }
        g_list_free_full(addr_strs, g_free);

        bgp_vrf_del_instance(vrf, afi, safi);
        return ERRCODE_SUCCESS;
    }

    inst = bgp_vrf_get_or_create_instance(vrf, afi, safi);
    return inst ? ERRCODE_SUCCESS : ERRCODE_FAIL;
}

uint32_t bgp_cfg_apply_neighbor(gboolean is_no, bgp_vrf_t *vrf, const net_addr_t *addr, uint32_t remote_as)
{
    if (!addr)
    {
        return ERRCODE_FAIL;
    }

    if (!vrf)
    {
        return is_no ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    if (is_no)
    {
        bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
        if (!sess)
        {
            return ERRCODE_SUCCESS;
        }

        bgp_vrf_af_disable_neighbor(vrf, BGP_AFI_IPV4, BGP_SAFI_UNICAST, addr);
        bgp_server_stop_session_conns(sess);
        bgp_vrf_del_session(vrf, addr);
        return ERRCODE_SUCCESS;
    }

    bgp_session_t *existing = bgp_vrf_find_session(vrf, addr);
    if (existing)
    {
        existing->remote_as = remote_as;
        return ERRCODE_SUCCESS;
    }

    bgp_session_t *sess = bgp_session_create(addr, remote_as, vrf);
    if (!sess)
    {
        return ERRCODE_FAIL;
    }

    bgp_vrf_add_session(vrf, sess);
    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_af_neighbor(gboolean is_no, bgp_vrf_t *vrf, bgp_afi_t afi, bgp_safi_t safi,
                                   const net_addr_t *addr)
{
    if (!addr)
    {
        return ERRCODE_FAIL;
    }

    if (!vrf)
    {
        return is_no ? ERRCODE_SUCCESS : ERRCODE_FAIL;
    }

    bgp_session_t *sess = bgp_vrf_find_session(vrf, addr);
    if (is_no)
    {
        if (!sess)
        {
            return ERRCODE_SUCCESS;
        }

        bgp_vrf_af_disable_neighbor(vrf, afi, safi, addr);
        if (!bgp_vrf_neighbor_has_any_af(vrf, addr))
        {
            bgp_server_stop_session_conns(sess);
        }
        return ERRCODE_SUCCESS;
    }

    if (!sess)
    {
        return ERRCODE_FAIL;
    }

    gboolean first_af = !bgp_vrf_neighbor_has_any_af(vrf, addr);
    if (bgp_vrf_af_enable_neighbor(vrf, afi, safi, addr) != 0)
    {
        return ERRCODE_FAIL;
    }

    if (first_af)
    {
        bgp_server_start_active_conn(sess);
    }

    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_router_id(gboolean is_no, bgp_vrf_t *vrf, const char *router_id)
{
    if (!vrf)
    {
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        vrf->router_id = 0;
        return ERRCODE_SUCCESS;
    }

    if (!router_id)
    {
        return ERRCODE_FAIL;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, router_id, &addr) != 1)
    {
        return ERRCODE_FAIL;
    }
    vrf->router_id = ntohl(addr.s_addr);
    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_timers(gboolean is_no, bgp_vrf_t *vrf, uint16_t keepalive, uint16_t hold_time)
{
    if (!vrf)
    {
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        vrf->keepalive = BGP_TIMER_DEFAULT_KEEPALIVE;
        vrf->hold_time = BGP_TIMER_DEFAULT_HOLD;
        return ERRCODE_SUCCESS;
    }

    vrf->keepalive = keepalive;
    vrf->hold_time = hold_time;
    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_connect_retry(gboolean is_no, bgp_vrf_t *vrf, uint16_t connect_retry)
{
    if (!vrf)
    {
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        vrf->connect_retry = BGP_TIMER_DEFAULT_CONNECT_RETRY;
        return ERRCODE_SUCCESS;
    }

    vrf->connect_retry = connect_retry;
    return ERRCODE_SUCCESS;
}

uint32_t bgp_cfg_apply_open_capability(gboolean is_no, bgp_session_t *sess, uint32_t cap_bit)
{
    if (!sess)
    {
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        BIT_CLR(sess->flags, cap_bit);
    }
    else
    {
        BIT_SET(sess->flags, cap_bit);
    }

    return ERRCODE_SUCCESS;
}
