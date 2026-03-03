/**
 * @file   bgp_peer.c
 * @brief  BGP per-AF peer（bgp_peer_t）实现
 * @author jhb
 * @date   2026/03/02
 */
#include "bgp_peer.h"

#include <glib.h>
#include <string.h>

#include "log.h"

// ============================================================================
// bgp_peer_t 生命周期（per-AF peer）
// ============================================================================

bgp_peer_t *bgp_peer_create(bgp_afi_t afi, bgp_safi_t safi, const net_addr_t *addr)
{
    bgp_peer_t *peer = g_malloc0(sizeof(bgp_peer_t));
    if (addr)
    {
        memcpy(&peer->addr, addr, sizeof(*addr));
    }
    peer->afi = afi;
    peer->safi = safi;
    peer->established = false;
    return peer;
}

void bgp_peer_destroy(bgp_peer_t *peer)
{
    if (!peer)
    {
        return;
    }
    g_free(peer);
}
