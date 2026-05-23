/**
 * @file   bgp_adj_rib_in.h
 * @brief  BGP Adj-RIB-In：peer 级别保存对端接收路径
 */
#ifndef BGP_ADJ_RIB_IN_H
#define BGP_ADJ_RIB_IN_H

#include <glib.h>
#include <stdbool.h>
#include <stdint.h>

#include "bgp.h"
#include "bgp_attr_intern.h"

typedef struct bgp_adj_rib_in bgp_adj_rib_in_t;
typedef struct bgp_session bgp_session_t;
typedef struct bgp_peer_update_ingest_stats bgp_peer_update_ingest_stats_t;

typedef struct bgp_adj_rib_in_entry
{
    bgp_attr_ref_t *attr_ref; /**< 接收属性（持有引用） */
    bgp_nexthop_t nexthop;    /**< 接收 nexthop */
} bgp_adj_rib_in_entry_t;

struct bgp_adj_rib_in
{
    GHashTable *table; /**< bgp_nlri_entry_t*(堆副本) -> bgp_adj_rib_in_entry_t* */
    uint32_t count;
};

typedef enum bgp_ari_change
{
    BGP_ARI_UNCHANGED = 0,
    BGP_ARI_UPDATED = 1,
    BGP_ARI_NEW = 2,
} bgp_ari_change_t;

bgp_adj_rib_in_t *bgp_adj_rib_in_create(void);
void bgp_adj_rib_in_destroy(bgp_adj_rib_in_t *ari);
void bgp_adj_rib_in_clear(bgp_adj_rib_in_t *ari);

const bgp_adj_rib_in_entry_t *bgp_adj_rib_in_lookup(const bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri);
bgp_ari_change_t bgp_adj_rib_in_update(bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri, bgp_attr_ref_t *attr_ref,
                                       const bgp_nexthop_t *nh);
bool bgp_adj_rib_in_remove(bgp_adj_rib_in_t *ari, const bgp_nlri_entry_t *nlri);
uint32_t bgp_adj_rib_in_count(const bgp_adj_rib_in_t *ari);

void bgp_adj_rib_in_clear_session(bgp_session_t *session);
void bgp_adj_rib_in_ingest_peer_update(bgp_session_t *session, const bgp_update_result_t *upd,
                                       bgp_peer_update_ingest_stats_t *stats);

#endif /* BGP_ADJ_RIB_IN_H */
