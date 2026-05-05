#include "fib_rib.h"

#include <string.h>

#include "net_addr.h"

typedef struct fib_route_key
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t safi;
    uint8_t prefix_len;
    net_addr_t prefix_addr;
} fib_route_key_t;

typedef struct fib_ilm_key
{
    uint32_t vrf_id;
    uint32_t in_label;
} fib_ilm_key_t;

static fib_route_key_t *route_key_create(const fib_route_entry_t *entry)
{
    if (!entry)
    {
        return NULL;
    }

    fib_route_key_t *key = g_malloc0(sizeof(*key));
    if (!key)
    {
        return NULL;
    }
    key->vrf_id = entry->vrf_id;
    key->afi = entry->afi;
    key->safi = entry->safi;
    key->prefix_len = entry->prefix_len;
    key->prefix_addr = entry->prefix_addr;
    (void)net_addr_prefix_normalize(&key->prefix_addr, key->prefix_len);
    return key;
}

static guint route_key_hash(gconstpointer p)
{
    const fib_route_key_t *key = (const fib_route_key_t *)p;
    if (!key)
    {
        return 0;
    }

    guint h = key->vrf_id;
    h = h * 33u + key->afi;
    h = h * 33u + key->safi;
    h = h * 33u + key->prefix_len;
    h ^= net_addr_hash(&key->prefix_addr);
    return h;
}

static gboolean route_key_equal(gconstpointer a, gconstpointer b)
{
    const fib_route_key_t *ka = (const fib_route_key_t *)a;
    const fib_route_key_t *kb = (const fib_route_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }

    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && ka->safi == kb->safi && ka->prefix_len == kb->prefix_len &&
           net_addr_equal(&ka->prefix_addr, &kb->prefix_addr);
}

static void route_state_free(gpointer p)
{
    g_free(p);
}

static void tunnel_state_free(gpointer p)
{
    g_free(p);
}

static fib_ilm_key_t *ilm_key_create(const fib_ilm_entry_t *entry)
{
    if (!entry)
    {
        return NULL;
    }

    fib_ilm_key_t *key = g_malloc0(sizeof(*key));
    if (!key)
    {
        return NULL;
    }
    key->vrf_id = entry->vrf_id;
    key->in_label = entry->in_label;
    return key;
}

static guint ilm_key_hash(gconstpointer p)
{
    const fib_ilm_key_t *key = (const fib_ilm_key_t *)p;
    if (!key)
    {
        return 0;
    }
    return key->vrf_id * 33u + key->in_label;
}

static gboolean ilm_key_equal(gconstpointer a, gconstpointer b)
{
    const fib_ilm_key_t *ka = (const fib_ilm_key_t *)a;
    const fib_ilm_key_t *kb = (const fib_ilm_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }
    return ka->vrf_id == kb->vrf_id && ka->in_label == kb->in_label;
}

static void ilm_state_free(gpointer p)
{
    g_free(p);
}

fib_rib_t *fib_rib_create(void)
{
    fib_rib_t *rib = g_malloc0(sizeof(*rib));
    if (!rib)
    {
        return NULL;
    }

    rib->routes = g_hash_table_new_full(route_key_hash, route_key_equal, g_free, route_state_free);
    rib->tunnels = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, tunnel_state_free);
    rib->ilms = g_hash_table_new_full(ilm_key_hash, ilm_key_equal, g_free, ilm_state_free);
    if (!rib->routes || !rib->tunnels || !rib->ilms)
    {
        fib_rib_destroy(rib);
        return NULL;
    }
    return rib;
}

void fib_rib_destroy(fib_rib_t *rib)
{
    if (!rib)
    {
        return;
    }
    if (rib->routes)
    {
        g_hash_table_destroy(rib->routes);
    }
    if (rib->tunnels)
    {
        g_hash_table_destroy(rib->tunnels);
    }
    if (rib->ilms)
    {
        g_hash_table_destroy(rib->ilms);
    }
    g_free(rib);
}

fib_route_state_t *fib_rib_route_lookup(fib_rib_t *rib, const fib_route_entry_t *entry)
{
    if (!rib || !rib->routes || !entry)
    {
        return NULL;
    }

    fib_route_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = entry->vrf_id;
    key.afi = entry->afi;
    key.safi = entry->safi;
    key.prefix_len = entry->prefix_len;
    key.prefix_addr = entry->prefix_addr;
    (void)net_addr_prefix_normalize(&key.prefix_addr, key.prefix_len);
    return (fib_route_state_t *)g_hash_table_lookup(rib->routes, &key);
}

fib_route_state_t *fib_rib_route_upsert(fib_rib_t *rib, const fib_route_entry_t *entry)
{
    if (!rib || !rib->routes || !entry)
    {
        return NULL;
    }

    fib_route_state_t *state = fib_rib_route_lookup(rib, entry);
    if (state)
    {
        uint8_t installed = state->installed;
        state->entry = *entry;
        state->installed = installed;
        return state;
    }

    fib_route_key_t *key = route_key_create(entry);
    state = g_malloc0(sizeof(*state));
    if (!key || !state)
    {
        g_free(key);
        g_free(state);
        return NULL;
    }
    state->entry = *entry;
    g_hash_table_insert(rib->routes, key, state);
    return state;
}

gboolean fib_rib_route_delete(fib_rib_t *rib, const fib_route_entry_t *entry, fib_route_entry_t *old_entry,
                              uint8_t *old_installed)
{
    fib_route_state_t *state = fib_rib_route_lookup(rib, entry);
    if (!rib || !rib->routes || !state)
    {
        return FALSE;
    }

    if (old_entry)
    {
        *old_entry = state->entry;
    }
    if (old_installed)
    {
        *old_installed = state->installed;
    }
    fib_route_key_t key;
    memset(&key, 0, sizeof(key));
    key.vrf_id = entry->vrf_id;
    key.afi = entry->afi;
    key.safi = entry->safi;
    key.prefix_len = entry->prefix_len;
    key.prefix_addr = entry->prefix_addr;
    (void)net_addr_prefix_normalize(&key.prefix_addr, key.prefix_len);
    return g_hash_table_remove(rib->routes, &key);
}

fib_tunnel_state_t *fib_rib_tunnel_lookup(fib_rib_t *rib, uint32_t tunnel_id)
{
    if (!rib || !rib->tunnels || tunnel_id == 0)
    {
        return NULL;
    }
    return (fib_tunnel_state_t *)g_hash_table_lookup(rib->tunnels, GUINT_TO_POINTER(tunnel_id));
}

fib_tunnel_state_t *fib_rib_tunnel_upsert(fib_rib_t *rib, const fib_tunnel_entry_t *entry)
{
    if (!rib || !rib->tunnels || !entry || entry->tunnel_id == 0)
    {
        return NULL;
    }

    fib_tunnel_state_t *state = fib_rib_tunnel_lookup(rib, entry->tunnel_id);
    if (!state)
    {
        state = g_malloc0(sizeof(*state));
        if (!state)
        {
            return NULL;
        }
        g_hash_table_insert(rib->tunnels, GUINT_TO_POINTER(entry->tunnel_id), state);
    }
    state->entry = *entry;
    return state;
}

gboolean fib_rib_tunnel_delete(fib_rib_t *rib, uint32_t tunnel_id)
{
    if (!rib || !rib->tunnels || tunnel_id == 0)
    {
        return FALSE;
    }
    return g_hash_table_remove(rib->tunnels, GUINT_TO_POINTER(tunnel_id));
}

fib_ilm_state_t *fib_rib_ilm_lookup(fib_rib_t *rib, uint32_t vrf_id, uint32_t in_label)
{
    if (!rib || !rib->ilms || in_label == 0)
    {
        return NULL;
    }

    fib_ilm_key_t key = {
        .vrf_id = vrf_id,
        .in_label = in_label,
    };
    return (fib_ilm_state_t *)g_hash_table_lookup(rib->ilms, &key);
}

fib_ilm_state_t *fib_rib_ilm_upsert(fib_rib_t *rib, const fib_ilm_entry_t *entry)
{
    if (!rib || !rib->ilms || !entry || entry->in_label == 0)
    {
        return NULL;
    }

    fib_ilm_state_t *state = fib_rib_ilm_lookup(rib, entry->vrf_id, entry->in_label);
    if (state)
    {
        uint8_t installed = state->installed;
        state->entry = *entry;
        state->installed = installed;
        return state;
    }

    fib_ilm_key_t *key = ilm_key_create(entry);
    state = g_malloc0(sizeof(*state));
    if (!key || !state)
    {
        g_free(key);
        g_free(state);
        return NULL;
    }
    state->entry = *entry;
    g_hash_table_insert(rib->ilms, key, state);
    return state;
}

gboolean fib_rib_ilm_delete(fib_rib_t *rib, const fib_ilm_entry_t *entry, fib_ilm_entry_t *old_entry,
                            uint8_t *old_installed)
{
    if (!rib || !rib->ilms || !entry)
    {
        return FALSE;
    }

    fib_ilm_state_t *state = fib_rib_ilm_lookup(rib, entry->vrf_id, entry->in_label);
    if (!state)
    {
        return FALSE;
    }
    if (old_entry)
    {
        *old_entry = state->entry;
    }
    if (old_installed)
    {
        *old_installed = state->installed;
    }

    fib_ilm_key_t key = {
        .vrf_id = entry->vrf_id,
        .in_label = entry->in_label,
    };
    return g_hash_table_remove(rib->ilms, &key);
}

void fib_rib_foreach_route(fib_rib_t *rib, GHFunc func, gpointer user_data)
{
    if (!rib || !rib->routes || !func)
    {
        return;
    }
    g_hash_table_foreach(rib->routes, func, user_data);
}

void fib_rib_foreach_ilm(fib_rib_t *rib, GHFunc func, gpointer user_data)
{
    if (!rib || !rib->ilms || !func)
    {
        return;
    }
    g_hash_table_foreach(rib->ilms, func, user_data);
}
