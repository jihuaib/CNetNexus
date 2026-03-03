/**
 * @file   bgp_session.c
 * @brief  BGP 会话结构生命周期实现
 * @author jhb
 * @date   2026/03/03
 */
#include "bgp_session.h"

#include <glib.h>
#include <string.h>

#include "log.h"

bgp_session_t *bgp_session_create(const net_addr_t *addr, uint32_t remote_as)
{
    bgp_session_t *sess = g_malloc0(sizeof(bgp_session_t));
    if (addr)
    {
        memcpy(&sess->neighbor_addr, addr, sizeof(*addr));
    }
    sess->remote_as = remote_as;
    bgp_conn_init(&sess->pri_conn);
    bgp_conn_init(&sess->sec_conn);
    sess->peers = NULL;

    char addr_str[64] = "";
    if (addr)
    {
        net_addr_to_str(addr, addr_str, sizeof(addr_str));
    }
    LOG_INFO("BGP 会话已创建: neighbor=%s AS=%u", addr_str, remote_as);
    return sess;
}

void bgp_session_destroy(bgp_session_t *session)
{
    if (!session)
    {
        return;
    }

    char addr_str[64];
    net_addr_to_str(&session->neighbor_addr, addr_str, sizeof(addr_str));
    LOG_INFO("BGP 会话已销毁: neighbor=%s", addr_str);

    bgp_conn_cleanup(&session->pri_conn);
    bgp_conn_cleanup(&session->sec_conn);

    /* peers 是借用引用，bgp_peer_t 由 bgp_instance_t 负责销毁，仅释放链表节点 */
    if (session->peers)
    {
        g_list_free(session->peers);
        session->peers = NULL;
    }
    g_free(session);
}
