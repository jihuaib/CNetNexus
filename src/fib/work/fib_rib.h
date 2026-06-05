/**
 * @file   fib_rib.h
 * @brief  FIB worker 内存状态：route/tunnel join 表
 */
#ifndef FIB_RIB_H
#define FIB_RIB_H

#include <glib.h>

#include "fib.h"

typedef struct fib_route_state
{
    fib_route_entry_t entry;
    uint8_t installed;
} fib_route_state_t;

typedef struct fib_tunnel_state
{
    fib_tunnel_entry_t entry;
} fib_tunnel_state_t;

typedef struct fib_nexthop_state
{
    fib_nexthop_entry_t entry;
} fib_nexthop_state_t;

typedef struct fib_ilm_state
{
    fib_ilm_entry_t entry;
    uint8_t installed;
} fib_ilm_state_t;

typedef struct fib_rib
{
    GHashTable *routes;
    GHashTable *tunnels;
    GHashTable *nexthops;
    GHashTable *ilms;
} fib_rib_t;

fib_rib_t *fib_rib_create(void);
void fib_rib_destroy(fib_rib_t *rib);

fib_route_state_t *fib_rib_route_upsert(fib_rib_t *rib, const fib_route_entry_t *entry);
fib_route_state_t *fib_rib_route_lookup(fib_rib_t *rib, const fib_route_entry_t *entry);
gboolean fib_rib_route_delete(fib_rib_t *rib, const fib_route_entry_t *entry, fib_route_entry_t *old_entry,
                              uint8_t *old_installed);

fib_tunnel_state_t *fib_rib_tunnel_upsert(fib_rib_t *rib, const fib_tunnel_entry_t *entry);
fib_tunnel_state_t *fib_rib_tunnel_lookup(fib_rib_t *rib, uint32_t tunnel_id);
gboolean fib_rib_tunnel_delete(fib_rib_t *rib, uint32_t tunnel_id);

fib_nexthop_state_t *fib_rib_nexthop_upsert(fib_rib_t *rib, const fib_nexthop_entry_t *entry);
fib_nexthop_state_t *fib_rib_nexthop_lookup(fib_rib_t *rib, uint32_t nexthop_id);
gboolean fib_rib_nexthop_delete(fib_rib_t *rib, uint32_t nexthop_id);

fib_ilm_state_t *fib_rib_ilm_upsert(fib_rib_t *rib, const fib_ilm_entry_t *entry);
fib_ilm_state_t *fib_rib_ilm_lookup(fib_rib_t *rib, uint32_t vrf_id, uint32_t in_label);
gboolean fib_rib_ilm_delete(fib_rib_t *rib, const fib_ilm_entry_t *entry, fib_ilm_entry_t *old_entry,
                            uint8_t *old_installed);

void fib_rib_foreach_route(fib_rib_t *rib, GHFunc func, gpointer user_data);
void fib_rib_foreach_ilm(fib_rib_t *rib, GHFunc func, gpointer user_data);
void fib_rib_foreach_nexthop(fib_rib_t *rib, GHFunc func, gpointer user_data);

#endif /* FIB_RIB_H */
