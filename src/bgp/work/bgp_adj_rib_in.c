/**
 * @file   bgp_adj_rib_in.c
 * @brief  BGP Adj-RIB-In 实现
 */
#include "bgp_adj_rib_in.h"

#include <string.h>

#include "bgp_ext_community.h"
#include "bgp_instance.h"
#include "bgp_peer.h"
#include "bgp_relay.h"
#include "bgp_session.h"
#include "bgp_worker.h"
#include "errcode.h"

static guint nlri_hash(gconstpointer p)
{
    const bgp_nlri_entry_t *nlri = p;
    const uint8_t *bytes = (const uint8_t *)nlri;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < sizeof(*nlri); i++)
    {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return (guint)h;
}

static gboolean nlri_equal(gconstpointer a, gconstpointer b)
{
    return bgp_nlri_equal((const bgp_nlri_entry_t *)a, (const bgp_nlri_entry_t *)b);
}

static void entry_free(gpointer p)
{
    bgp_adj_rib_in_entry_t *entry = (bgp_adj_rib_in_entry_t *)p;
    if (!entry)
    {
        return;
    }
    bgp_attr_release(entry->attr_ref);
    g_free(entry);
}

bgp_adj_rib_in_t *bgp_adj_rib_in_create(void)
{
    bgp_adj_rib_in_t *ari = g_malloc0(sizeof(*ari));
    ari->table = g_hash_table_new_full(nlri_hash, nlri_equal, g_free, entry_free);
    return ari;
}

void bgp_adj_rib_in_destroy(bgp_adj_rib_in_t *ari)
{
    if (!ari)
    {
        return;
    }
    if (ari->table)
    {
        g_hash_table_destroy(ari->table);
        ari->table = NULL;
    }
    g_free(ari);
}

void bgp_adj_rib_in_clear(bgp_adj_rib_in_t *ari)
{
    if (!ari || !ari->table)
    {
        return;
    }
    g_hash_table_remove_all(ari->table);
    ari->count = 0;
}

const bgp_adj_rib_in_entry_t *bgp_adj_rib_in_lookup(const bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri)
{
    if (!ari || !ari->table || !nlri)
    {
        return NULL;
    }
    return (const bgp_adj_rib_in_entry_t *)g_hash_table_lookup(ari->table, nlri);
}

bgp_ari_change_t bgp_adj_rib_in_update(bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri, bgp_attr_ref_t *attr_ref,
                                       const bgp_nexthop_t *nh)
{
    if (!ari || !ari->table || !nlri || !attr_ref || !nh)
    {
        return BGP_ARI_UNCHANGED;
    }

    bgp_adj_rib_in_entry_t *existing = (bgp_adj_rib_in_entry_t *)g_hash_table_lookup(ari->table, nlri);
    if (existing)
    {
        gboolean attr_same = (existing->attr_ref == attr_ref) ||
                             (existing->attr_ref && existing->attr_ref->attr_id == attr_ref->attr_id);
        gboolean nh_same = memcmp(&existing->nexthop, nh, sizeof(*nh)) == 0;
        if (attr_same && nh_same)
        {
            return BGP_ARI_UNCHANGED;
        }

        bgp_attr_ref_get(attr_ref);
        bgp_attr_ref_t *old_ref = existing->attr_ref;
        existing->attr_ref = attr_ref;
        existing->nexthop = *nh;
        bgp_attr_release(old_ref);
        return BGP_ARI_UPDATED;
    }

    bgp_nlri_entry_t *key = g_malloc(sizeof(*key));
    memcpy(key, nlri, sizeof(*key));

    bgp_adj_rib_in_entry_t *entry = g_malloc0(sizeof(*entry));
    bgp_attr_ref_get(attr_ref);
    entry->attr_ref = attr_ref;
    entry->nexthop = *nh;

    g_hash_table_insert(ari->table, key, entry);
    ari->count++;
    return BGP_ARI_NEW;
}

bool bgp_adj_rib_in_remove(bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri)
{
    if (!ari || !ari->table || !nlri)
    {
        return false;
    }
    if (!g_hash_table_remove(ari->table, nlri))
    {
        return false;
    }
    if (ari->count > 0)
    {
        ari->count--;
    }
    return true;
}

uint32_t bgp_adj_rib_in_count(const bgp_adj_rib_in_t *ari)
{
    return ari ? ari->count : 0;
}

static bgp_peer_t *find_peer_for_nlri(bgp_session_t *session, const bgp_nlri_entry_t *nlri)
{
    if (!session || !nlri)
    {
        return NULL;
    }

    for (GList *l = session->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (!peer || !peer->inst)
        {
            continue;
        }
        if ((uint16_t)peer->inst->afi == nlri->afi && (uint8_t)peer->inst->safi == nlri->safi)
        {
            return peer;
        }
    }
    return NULL;
}

void bgp_adj_rib_in_clear_session(bgp_session_t *session)
{
    if (!session)
    {
        return;
    }
    for (GList *l = session->peer_list; l; l = l->next)
    {
        bgp_peer_t *peer = (bgp_peer_t *)l->data;
        if (peer && peer->rib_in)
        {
            bgp_adj_rib_in_clear(peer->rib_in);
        }
    }
}

void bgp_adj_rib_in_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                       bgp_peer_update_ingest_stats_t *stats)
{
    if (stats)
    {
        memset(stats, 0, sizeof(*stats));
    }
    if (!session || !session->vrf || !upd)
    {
        return;
    }

    bgp_attr_t ribin_attr = upd->attr;
    bgp_ext_community_merge_vrf_export_rts(&ribin_attr, session->vrf->vrf_id, upd->afi);

    GArray *reach = g_array_new(FALSE, FALSE, sizeof(bgp_nlri_entry_t));
    GArray *unreach = g_array_new(FALSE, FALSE, sizeof(bgp_nlri_entry_t));
    uint32_t reach_failed = 0;
    uint32_t unreach_failed = 0;

    for (uint32_t i = 0; i < upd->reach_len; i++)
    {
        const bgp_nlri_entry_t *nlri = &upd->reach[i];
        bgp_peer_t *peer = find_peer_for_nlri(session, nlri);
        if (!peer || !peer->rib_in || !peer->inst)
        {
            reach_failed++;
            continue;
        }
        bgp_attr_ref_t *attr_ref = bgp_attr_intern(peer->inst, &ribin_attr);
        if (!attr_ref)
        {
            reach_failed++;
            continue;
        }
        (void)bgp_adj_rib_in_update(peer->rib_in, nlri, attr_ref, &upd->nexthop);
        bgp_attr_release(attr_ref);
        g_array_append_val(reach, *nlri);
    }

    for (uint32_t i = 0; i < upd->unreach_len; i++)
    {
        const bgp_nlri_entry_t *nlri = &upd->unreach[i];
        bgp_peer_t *peer = find_peer_for_nlri(session, nlri);
        if (!peer || !peer->rib_in)
        {
            unreach_failed++;
            continue;
        }
        (void)bgp_adj_rib_in_remove(peer->rib_in, nlri);
        g_array_append_val(unreach, *nlri);
    }

    bgp_update_result_t filtered = *upd;
    const bgp_attr_t *base_attr = &upd->attr;
    filtered.attr = ribin_attr;
    filtered.reach = (bgp_nlri_entry_t *)reach->data;
    filtered.reach_len = reach->len;
    filtered.unreach = (bgp_nlri_entry_t *)unreach->data;
    filtered.unreach_len = unreach->len;

    bgp_peer_update_ingest_stats_t relay_stats = {0};
    bgp_relay_ingest_peer_update(session, &filtered, base_attr, &relay_stats);
    relay_stats.reach_failed += reach_failed;
    relay_stats.unreach_failed += unreach_failed;

    if (stats)
    {
        *stats = relay_stats;
    }

    g_array_free(reach, TRUE);
    g_array_free(unreach, TRUE);
}
