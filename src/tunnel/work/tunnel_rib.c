#include "tunnel_rib.h"

#include <net/if.h>
#include <string.h>

#include "errcode.h"
#include "fib.h"
#include "log.h"
#include "tunnel_config.h"
#include "tunnel_main.h"

#define TUNNEL_RESOLVE_MAX_DEPTH 16u

typedef struct tunnel_watch
{
    uint32_t owner_module_id;
    tunnel_resolve_req_t req;
    int last_resolved;
    tunnel_resolve_notify_t last_notify;
} tunnel_watch_t;

typedef struct tunnel_id_key
{
    uint32_t vrf_id;
    uint16_t afi;
    uint8_t _pad[2];
    net_addr_t endpoint;
} tunnel_id_key_t;

typedef struct tunnel_nhlfe
{
    uint32_t id;
    net_addr_t endpoint;
    net_addr_t relay_addr;
    uint32_t out_ifindex;
    uint8_t source_type;
    uint8_t label_count;
    uint32_t labels[TUNNEL_MAX_LABEL_STACK];
} tunnel_nhlfe_t;

typedef struct tunnel_ftn
{
    tunnel_fec_t fec;
    uint32_t nhlfe_id;
    uint8_t source_type;
    uint8_t state;
} tunnel_ftn_t;

typedef struct tunnel_ilm
{
    uint32_t vrf_id;
    uint32_t in_label;
    uint32_t nhlfe_id;
    uint32_t out_ifindex;
    uint8_t action;
    uint8_t state;
} tunnel_ilm_t;

typedef struct tunnel_label_binding
{
    tunnel_label_req_t req;
    uint32_t label;
} tunnel_label_binding_t;

struct tunnel_rib
{
    GList *candidates;
    GList *watches;
    GList *label_bindings;
    GList *nhlfes;
    GList *ftns;
    GList *ilms;
    GHashTable *tunnel_ids;
    /* Tunnel IDs are also used as NHLFE IDs for the resolved endpoint. */
    uint32_t next_tunnel_id;
    uint32_t next_label;
    uint32_t label_dynamic_min;
    uint32_t label_dynamic_max;
};

static guint tunnel_id_key_hash(gconstpointer p)
{
    const tunnel_id_key_t *key = (const tunnel_id_key_t *)p;
    if (!key)
    {
        return 0;
    }

    guint h = key->vrf_id;
    h = h * 33u + key->afi;
    h ^= net_addr_hash(&key->endpoint);
    return h;
}

static gboolean tunnel_id_key_equal(gconstpointer a, gconstpointer b)
{
    const tunnel_id_key_t *ka = (const tunnel_id_key_t *)a;
    const tunnel_id_key_t *kb = (const tunnel_id_key_t *)b;
    if (!ka || !kb)
    {
        return FALSE;
    }
    return ka->vrf_id == kb->vrf_id && ka->afi == kb->afi && net_addr_equal(&ka->endpoint, &kb->endpoint);
}

static void tunnel_id_make_key(tunnel_id_key_t *key, uint32_t vrf_id, uint16_t afi, const net_addr_t *endpoint)
{
    memset(key, 0, sizeof(*key));
    key->vrf_id = vrf_id;
    key->afi = afi;
    key->endpoint = *endpoint;
}

static gboolean tunnel_id_value_is_used(const tunnel_rib_t *rib, uint32_t tunnel_id)
{
    if (!rib || !rib->tunnel_ids || tunnel_id == 0u)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, rib->tunnel_ids);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)key;
        if (GPOINTER_TO_UINT(value) == tunnel_id)
        {
            return TRUE;
        }
    }

    return FALSE;
}

static uint32_t tunnel_id_alloc(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return 0u;
    }

    if (rib->next_tunnel_id == 0u)
    {
        rib->next_tunnel_id = 1u;
    }

    uint32_t start = rib->next_tunnel_id;
    do
    {
        uint32_t tunnel_id = rib->next_tunnel_id++;
        if (rib->next_tunnel_id == 0u)
        {
            rib->next_tunnel_id = 1u;
        }

        if (!tunnel_id_value_is_used(rib, tunnel_id))
        {
            return tunnel_id;
        }
    } while (rib->next_tunnel_id != start);

    return 0u;
}

static uint32_t tunnel_id_lookup_or_alloc(tunnel_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *endpoint,
                                          gboolean create)
{
    if (!rib || !rib->tunnel_ids || !endpoint || endpoint->family == 0)
    {
        return 0u;
    }

    tunnel_id_key_t lookup;
    tunnel_id_make_key(&lookup, vrf_id, afi, endpoint);

    gpointer value = g_hash_table_lookup(rib->tunnel_ids, &lookup);
    if (value)
    {
        return GPOINTER_TO_UINT(value);
    }
    if (!create)
    {
        return 0u;
    }

    tunnel_id_key_t *stored = g_malloc0(sizeof(*stored));
    if (!stored)
    {
        return 0u;
    }
    *stored = lookup;

    uint32_t tunnel_id = tunnel_id_alloc(rib);
    if (tunnel_id == 0u)
    {
        g_free(stored);
        return 0u;
    }
    g_hash_table_insert(rib->tunnel_ids, stored, GUINT_TO_POINTER(tunnel_id));
    return tunnel_id;
}

static gboolean tunnel_endpoint_equal(uint32_t vrf_id, uint16_t afi, const net_addr_t *addr,
                                      const tunnel_candidate_t *candidate)
{
    return candidate && candidate->vrf_id == vrf_id && candidate->afi == afi &&
           net_addr_equal(addr, &candidate->endpoint);
}

static gboolean tunnel_candidate_same_owner(const tunnel_candidate_t *a, const tunnel_candidate_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }

    return a->vrf_id == b->vrf_id && a->afi == b->afi && a->source_type == b->source_type &&
           a->owner_module_id == b->owner_module_id && a->owner_id == b->owner_id &&
           net_addr_equal(&a->endpoint, &b->endpoint);
}

static gboolean tunnel_fec_equal(const tunnel_fec_t *a, const tunnel_fec_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }

    return a->vrf_id == b->vrf_id && a->afi == b->afi && a->prefix_len == b->prefix_len &&
           net_addr_equal(&a->addr, &b->addr);
}

static gboolean tunnel_label_req_same_owner(const tunnel_label_req_t *a, const tunnel_label_req_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }

    return a->vrf_id == b->vrf_id && a->afi == b->afi && a->source_type == b->source_type &&
           a->owner_module_id == b->owner_module_id && a->owner_id == b->owner_id && tunnel_fec_equal(&a->fec, &b->fec);
}

static gboolean tunnel_label_is_used(const tunnel_rib_t *rib, uint32_t label)
{
    for (const GList *it = rib ? rib->label_bindings : NULL; it; it = it->next)
    {
        const tunnel_label_binding_t *binding = it->data;
        if (binding && binding->label == label)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static uint32_t tunnel_label_next(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return 0;
    }

    if (rib->next_label < rib->label_dynamic_min || rib->next_label > rib->label_dynamic_max)
    {
        rib->next_label = rib->label_dynamic_min;
    }

    uint32_t start = rib->next_label;
    do
    {
        uint32_t label = rib->next_label++;
        if (rib->next_label > rib->label_dynamic_max)
        {
            rib->next_label = rib->label_dynamic_min;
        }
        if (!tunnel_label_is_used(rib, label))
        {
            return label;
        }
    } while (rib->next_label != start);

    return 0;
}

static tunnel_label_binding_t *tunnel_label_binding_lookup(const tunnel_rib_t *rib, const tunnel_label_req_t *req)
{
    for (const GList *it = rib ? rib->label_bindings : NULL; it; it = it->next)
    {
        tunnel_label_binding_t *binding = it->data;
        if (binding && tunnel_label_req_same_owner(&binding->req, req))
        {
            return binding;
        }
    }
    return NULL;
}

static gboolean tunnel_ifname_is_logical_loop(const char *ifname)
{
    if (!ifname || strncmp(ifname, "loop", 4) != 0 || ifname[4] == '\0')
    {
        return FALSE;
    }

    for (const char *p = ifname + 4; *p; ++p)
    {
        if (*p < '0' || *p > '9')
        {
            return FALSE;
        }
    }
    return TRUE;
}

static uint32_t tunnel_local_pop_out_ifindex(uint32_t out_ifindex)
{
    if (out_ifindex == 0u)
    {
        return 0u;
    }

    char ifname[IF_NAMESIZE];
    if (!if_indextoname(out_ifindex, ifname) || !tunnel_ifname_is_logical_loop(ifname))
    {
        return out_ifindex;
    }

    unsigned int lo_ifindex = if_nametoindex("lo");
    return lo_ifindex ? (uint32_t)lo_ifindex : out_ifindex;
}

static void tunnel_append_ilm_for_fec(tunnel_rib_t *rib, const tunnel_fec_t *fec, uint32_t nhlfe_id, uint8_t state)
{
    if (!rib || !fec)
    {
        return;
    }

    for (GList *it = rib->label_bindings; it; it = it->next)
    {
        tunnel_label_binding_t *binding = it->data;
        if (!binding || !tunnel_fec_equal(&binding->req.fec, fec))
        {
            continue;
        }

        tunnel_ilm_t *ilm = g_malloc0(sizeof(*ilm));
        if (!ilm)
        {
            continue;
        }
        ilm->vrf_id = fec->vrf_id;
        ilm->in_label = binding->label;
        ilm->nhlfe_id = nhlfe_id;
        ilm->action = state ? TUNNEL_ACTION_SWAP : TUNNEL_ACTION_DROP;
        ilm->state = state;
        rib->ilms = g_list_prepend(rib->ilms, ilm);
    }
}

static gboolean tunnel_ilm_exists_for_label(const tunnel_rib_t *rib, uint32_t vrf_id, uint32_t in_label)
{
    for (const GList *it = rib ? rib->ilms : NULL; it; it = it->next)
    {
        const tunnel_ilm_t *ilm = it->data;
        if (ilm && ilm->vrf_id == vrf_id && ilm->in_label == in_label)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static const tunnel_nhlfe_t *tunnel_nhlfe_lookup(const tunnel_rib_t *rib, uint32_t nhlfe_id)
{
    if (!rib || nhlfe_id == 0u)
    {
        return NULL;
    }

    for (const GList *it = rib->nhlfes; it; it = it->next)
    {
        const tunnel_nhlfe_t *nhlfe = it->data;
        if (nhlfe && nhlfe->id == nhlfe_id)
        {
            return nhlfe;
        }
    }
    return NULL;
}

static void tunnel_fill_fib_ilm(fib_ilm_entry_t *entry, const tunnel_rib_t *rib, const tunnel_ilm_t *ilm)
{
    if (!entry || !ilm)
    {
        return;
    }

    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = ilm->vrf_id;
    entry->in_label = ilm->in_label;
    entry->action = ilm->action;
    entry->state = ilm->state;
    entry->nhlfe_id = ilm->nhlfe_id;
    entry->out_ifindex = ilm->out_ifindex;

    if (ilm->action == TUNNEL_ACTION_SWAP)
    {
        const tunnel_nhlfe_t *nhlfe = tunnel_nhlfe_lookup(rib, ilm->nhlfe_id);
        if (nhlfe)
        {
            entry->out_ifindex = nhlfe->out_ifindex;
            entry->relay_addr = nhlfe->relay_addr;
            entry->label_count = nhlfe->label_count;
            memcpy(entry->labels, nhlfe->labels, sizeof(entry->labels));
        }
        else
        {
            entry->state = 0u;
        }
    }
}

static void tunnel_sync_fib_ilm_delete(const tunnel_ilm_t *ilm)
{
    if (!ilm)
    {
        return;
    }

    fib_ilm_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.vrf_id = ilm->vrf_id;
    entry.in_label = ilm->in_label;
    (void)fib_rpc_ilm_delete(tunnel_local_ipc_ctx(), &entry);
}

static void tunnel_sync_fib_ilm_upsert(const tunnel_rib_t *rib, const tunnel_ilm_t *ilm)
{
    if (!ilm)
    {
        return;
    }

    fib_ilm_entry_t entry;
    tunnel_fill_fib_ilm(&entry, rib, ilm);
    (void)fib_rpc_ilm_upsert(tunnel_local_ipc_ctx(), &entry);
}

static void tunnel_sync_fib_ilm_delete_all(const tunnel_rib_t *rib)
{
    for (const GList *it = rib ? rib->ilms : NULL; it; it = it->next)
    {
        tunnel_sync_fib_ilm_delete((const tunnel_ilm_t *)it->data);
    }
}

static void tunnel_sync_fib_ilm_upsert_all(const tunnel_rib_t *rib)
{
    for (const GList *it = rib ? rib->ilms : NULL; it; it = it->next)
    {
        tunnel_sync_fib_ilm_upsert(rib, (const tunnel_ilm_t *)it->data);
    }
}

static void tunnel_append_local_pop_ilms(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return;
    }

    for (const GList *it = rib->label_bindings; it; it = it->next)
    {
        const tunnel_label_binding_t *binding = it->data;
        if (!binding || tunnel_ilm_exists_for_label(rib, binding->req.vrf_id, binding->label))
        {
            continue;
        }

        tunnel_ilm_t *ilm = g_malloc0(sizeof(*ilm));
        if (!ilm)
        {
            continue;
        }
        ilm->vrf_id = binding->req.vrf_id;
        ilm->in_label = binding->label;
        ilm->nhlfe_id = 0u;
        ilm->out_ifindex = tunnel_local_pop_out_ifindex(binding->req.out_ifindex);
        ilm->action = TUNNEL_ACTION_POP;
        ilm->state = 1u;
        rib->ilms = g_list_prepend(rib->ilms, ilm);
    }
}

static tunnel_candidate_t *tunnel_candidate_best(const tunnel_rib_t *rib, uint32_t vrf_id, uint16_t afi,
                                                 const net_addr_t *endpoint)
{
    tunnel_candidate_t *best = NULL;

    for (const GList *it = rib ? rib->candidates : NULL; it; it = it->next)
    {
        tunnel_candidate_t *candidate = it->data;
        if (!tunnel_endpoint_equal(vrf_id, afi, endpoint, candidate))
        {
            continue;
        }

        if (!best || candidate->preference < best->preference ||
            (candidate->preference == best->preference && candidate->source_type < best->source_type))
        {
            best = candidate;
        }
    }

    return best;
}

static gboolean tunnel_stack_append(tunnel_resolve_notify_t *notify, const tunnel_candidate_t *candidate)
{
    if (!notify || !candidate)
    {
        return FALSE;
    }

    if ((uint32_t)notify->label_count + candidate->label_count > TUNNEL_MAX_LABEL_STACK)
    {
        return FALSE;
    }

    for (uint8_t i = 0; i < candidate->label_count; i++)
    {
        notify->labels[notify->label_count++] = candidate->labels[i];
    }
    return TRUE;
}

static gboolean tunnel_resolve_inner(const tunnel_rib_t *rib, uint32_t vrf_id, uint16_t afi, const net_addr_t *endpoint,
                                     tunnel_resolve_notify_t *notify, uint32_t depth)
{
    if (!rib || !endpoint || !notify || depth >= TUNNEL_RESOLVE_MAX_DEPTH)
    {
        return FALSE;
    }

    tunnel_candidate_t *candidate = tunnel_candidate_best(rib, vrf_id, afi, endpoint);
    if (!candidate)
    {
        return FALSE;
    }

    if (candidate->out_ifindex != 0)
    {
        notify->resolved = 1;
        notify->source_type = candidate->source_type;
        notify->out_ifindex = candidate->out_ifindex;
        notify->relay_addr = candidate->relay_addr;
        if (net_addr_is_zero(&notify->relay_addr))
        {
            notify->relay_addr = candidate->endpoint;
        }
        return tunnel_stack_append(notify, candidate);
    }

    if (net_addr_is_zero(&candidate->nexthop) || net_addr_equal(&candidate->nexthop, endpoint))
    {
        return FALSE;
    }

    if (!tunnel_resolve_inner(rib, vrf_id, afi, &candidate->nexthop, notify, depth + 1))
    {
        return FALSE;
    }

    return tunnel_stack_append(notify, candidate);
}

static tunnel_resolve_notify_t tunnel_resolve(tunnel_rib_t *rib, const tunnel_resolve_req_t *req)
{
    tunnel_resolve_notify_t notify;
    memset(&notify, 0, sizeof(notify));

    if (!req)
    {
        return notify;
    }

    notify.vrf_id = req->vrf_id;
    notify.afi = req->afi;
    notify.endpoint = req->endpoint;
    (void)tunnel_resolve_inner(rib, req->vrf_id, req->afi, &req->endpoint, &notify, 0);
    notify.tunnel_id =
        tunnel_id_lookup_or_alloc(rib, req->vrf_id, req->afi, &req->endpoint, notify.resolved ? TRUE : FALSE);
    return notify;
}

static void tunnel_sync_fib(const tunnel_resolve_notify_t *notify)
{
    if (!notify || notify->tunnel_id == 0u)
    {
        return;
    }

    fib_tunnel_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.tunnel_id = notify->tunnel_id;

    if (!notify->resolved)
    {
        (void)fib_rpc_tunnel_delete(tunnel_local_ipc_ctx(), &entry);
        return;
    }

    entry.state = 1u;
    entry.label_count = notify->label_count;
    entry.out_ifindex = notify->out_ifindex;
    entry.relay_addr = notify->relay_addr;
    memcpy(entry.labels, notify->labels, sizeof(entry.labels));
    (void)fib_rpc_tunnel_upsert(tunnel_local_ipc_ctx(), &entry);
}

static void tunnel_notify_watch(tunnel_watch_t *watch, const tunnel_resolve_notify_t *notify)
{
    if (!watch || !notify)
    {
        return;
    }

    tunnel_resolve_notify_t *payload = g_memdup2(notify, sizeof(*notify));
    if (!payload)
    {
        return;
    }

    dev_ipc_message_t *msg = dev_ipc_message_create(TUNNEL_MSG_TYPE_RESOLVE_NOTIFY, DEV_MODULE_ID_TUNNEL,
                                                    watch->owner_module_id, 0, payload, sizeof(*payload), g_free);
    if (!msg)
    {
        g_free(payload);
        return;
    }

    if (dev_ipc_send(tunnel_local_ipc_ctx(), watch->owner_module_id, msg) != 0)
    {
        LOG_WARN("TUNNEL: resolve notify send failed, owner=%u", watch->owner_module_id);
    }
    dev_ipc_message_free(msg);
}

static gboolean tunnel_notify_equal(const tunnel_resolve_notify_t *a, const tunnel_resolve_notify_t *b)
{
    if (!a || !b)
    {
        return FALSE;
    }

    return memcmp(a, b, sizeof(*a)) == 0;
}

static void tunnel_rebuild_forwarding_tables(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return;
    }

    tunnel_sync_fib_ilm_delete_all(rib);

    g_list_free_full(rib->nhlfes, g_free);
    g_list_free_full(rib->ftns, g_free);
    g_list_free_full(rib->ilms, g_free);
    rib->nhlfes = NULL;
    rib->ftns = NULL;
    rib->ilms = NULL;

    for (GList *it = rib->candidates; it; it = it->next)
    {
        tunnel_candidate_t *candidate = it->data;
        tunnel_resolve_req_t req = {
            .vrf_id = candidate->vrf_id,
            .afi = candidate->afi,
            .endpoint = candidate->endpoint,
        };
        tunnel_resolve_notify_t resolved = tunnel_resolve(rib, &req);
        if (!resolved.resolved)
        {
            continue;
        }
        if (resolved.tunnel_id == 0u)
        {
            continue;
        }

        tunnel_nhlfe_t *nhlfe = g_malloc0(sizeof(*nhlfe));
        if (!nhlfe)
        {
            continue;
        }
        nhlfe->id = resolved.tunnel_id;
        nhlfe->endpoint = candidate->endpoint;
        nhlfe->relay_addr = resolved.relay_addr;
        nhlfe->out_ifindex = resolved.out_ifindex;
        nhlfe->source_type = resolved.source_type;
        nhlfe->label_count = resolved.label_count;
        memcpy(nhlfe->labels, resolved.labels, sizeof(nhlfe->labels));
        rib->nhlfes = g_list_prepend(rib->nhlfes, nhlfe);

        if (candidate->fec.addr.family != 0)
        {
            tunnel_ftn_t *ftn = g_malloc0(sizeof(*ftn));
            if (ftn)
            {
                ftn->fec = candidate->fec;
                ftn->nhlfe_id = nhlfe->id;
                ftn->source_type = candidate->source_type;
                ftn->state = 1;
                rib->ftns = g_list_prepend(rib->ftns, ftn);
            }
        }

        if (candidate->fec.addr.family != 0)
        {
            tunnel_append_ilm_for_fec(rib, &candidate->fec, nhlfe->id, 1);
        }
    }

    tunnel_append_local_pop_ilms(rib);
    tunnel_sync_fib_ilm_upsert_all(rib);
}

tunnel_rib_t *tunnel_rib_create(void)
{
    tunnel_config_t cfg;
    if (tunnel_config_load(&cfg) != ERRCODE_SUCCESS)
    {
        return NULL;
    }

    tunnel_rib_t *rib = g_malloc0(sizeof(*rib));
    if (rib)
    {
        rib->tunnel_ids = g_hash_table_new_full(tunnel_id_key_hash, tunnel_id_key_equal, g_free, NULL);
        rib->next_tunnel_id = 1;
        rib->label_dynamic_min = cfg.label_dynamic_min;
        rib->label_dynamic_max = cfg.label_dynamic_max;
        rib->next_label = rib->label_dynamic_min;
    }
    return rib;
}

void tunnel_rib_destroy(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return;
    }

    g_list_free_full(rib->candidates, g_free);
    g_list_free_full(rib->watches, g_free);
    g_list_free_full(rib->label_bindings, g_free);
    g_list_free_full(rib->nhlfes, g_free);
    g_list_free_full(rib->ftns, g_free);
    g_list_free_full(rib->ilms, g_free);
    if (rib->tunnel_ids)
    {
        g_hash_table_destroy(rib->tunnel_ids);
    }
    g_free(rib);
}

int tunnel_rib_candidate_add(tunnel_rib_t *rib, const tunnel_candidate_t *candidate)
{
    if (!rib || !candidate || candidate->endpoint.family == 0)
    {
        return ERRCODE_FAIL;
    }

    (void)tunnel_rib_candidate_del(rib, candidate);

    tunnel_candidate_t *copy = g_memdup2(candidate, sizeof(*candidate));
    if (!copy)
    {
        return ERRCODE_FAIL;
    }
    if (copy->preference == 0)
    {
        copy->preference = 100;
    }
    if (copy->owner_module_id == 0)
    {
        copy->owner_module_id = DEV_MODULE_ID_TUNNEL;
    }
    rib->candidates = g_list_prepend(rib->candidates, copy);
    return ERRCODE_SUCCESS;
}

int tunnel_rib_label_alloc(tunnel_rib_t *rib, const tunnel_label_req_t *req, uint32_t *label_out)
{
    if (!rib || !req || !label_out || req->owner_module_id == 0 || req->fec.addr.family == 0)
    {
        return ERRCODE_FAIL;
    }

    tunnel_label_binding_t *binding = tunnel_label_binding_lookup(rib, req);
    if (binding)
    {
        binding->req = *req;
        *label_out = binding->label;
        return ERRCODE_SUCCESS;
    }

    uint32_t label = tunnel_label_next(rib);
    if (label == 0)
    {
        return ERRCODE_FAIL;
    }

    binding = g_malloc0(sizeof(*binding));
    if (!binding)
    {
        return ERRCODE_FAIL;
    }

    binding->req = *req;
    binding->label = label;
    rib->label_bindings = g_list_prepend(rib->label_bindings, binding);
    *label_out = label;
    return ERRCODE_SUCCESS;
}

int tunnel_rib_label_release(tunnel_rib_t *rib, const tunnel_label_req_t *req)
{
    if (!rib || !req)
    {
        return ERRCODE_FAIL;
    }

    for (GList *it = rib->label_bindings; it; it = it->next)
    {
        tunnel_label_binding_t *binding = it->data;
        if (binding && tunnel_label_req_same_owner(&binding->req, req))
        {
            rib->label_bindings = g_list_delete_link(rib->label_bindings, it);
            g_free(binding);
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_SUCCESS;
}

int tunnel_rib_candidate_del(tunnel_rib_t *rib, const tunnel_candidate_t *candidate)
{
    if (!rib || !candidate)
    {
        return ERRCODE_FAIL;
    }

    for (GList *it = rib->candidates; it; it = it->next)
    {
        tunnel_candidate_t *entry = it->data;
        if (tunnel_candidate_same_owner(entry, candidate))
        {
            rib->candidates = g_list_delete_link(rib->candidates, it);
            g_free(entry);
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_SUCCESS;
}

int tunnel_rib_watch_add(tunnel_rib_t *rib, uint32_t owner_module_id, const tunnel_resolve_req_t *req)
{
    if (!rib || !req || req->endpoint.family == 0 || owner_module_id == 0)
    {
        return ERRCODE_FAIL;
    }

    for (GList *it = rib->watches; it; it = it->next)
    {
        tunnel_watch_t *watch = it->data;
        if (watch->owner_module_id == owner_module_id && watch->req.vrf_id == req->vrf_id &&
            watch->req.afi == req->afi && net_addr_equal(&watch->req.endpoint, &req->endpoint))
        {
            return ERRCODE_SUCCESS;
        }
    }

    tunnel_watch_t *watch = g_malloc0(sizeof(*watch));
    if (!watch)
    {
        return ERRCODE_FAIL;
    }
    watch->owner_module_id = owner_module_id;
    watch->req = *req;
    watch->last_resolved = -1;
    rib->watches = g_list_prepend(rib->watches, watch);
    return ERRCODE_SUCCESS;
}

int tunnel_rib_watch_del(tunnel_rib_t *rib, uint32_t owner_module_id, const tunnel_resolve_req_t *req)
{
    if (!rib || !req)
    {
        return ERRCODE_FAIL;
    }

    for (GList *it = rib->watches; it; it = it->next)
    {
        tunnel_watch_t *watch = it->data;
        if (watch->owner_module_id == owner_module_id && watch->req.vrf_id == req->vrf_id &&
            watch->req.afi == req->afi && net_addr_equal(&watch->req.endpoint, &req->endpoint))
        {
            rib->watches = g_list_delete_link(rib->watches, it);
            g_free(watch);
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_SUCCESS;
}

void tunnel_rib_recompute(tunnel_rib_t *rib)
{
    if (!rib)
    {
        return;
    }

    tunnel_rebuild_forwarding_tables(rib);

    for (GList *it = rib->watches; it; it = it->next)
    {
        tunnel_watch_t *watch = it->data;
        tunnel_resolve_notify_t notify = tunnel_resolve(rib, &watch->req);
        if (watch->last_resolved != (int)notify.resolved || !tunnel_notify_equal(&watch->last_notify, &notify))
        {
            tunnel_sync_fib(&notify);
            tunnel_notify_watch(watch, &notify);
            watch->last_resolved = notify.resolved;
            watch->last_notify = notify;
        }
    }
}

static const char *source_name(uint8_t source_type)
{
    switch (source_type)
    {
        case TUNNEL_SOURCE_BGP_ADJ:
            return "bgp-adj";
        case TUNNEL_SOURCE_BGP_LU:
            return "bgp-lu";
        case TUNNEL_SOURCE_LDP:
            return "ldp";
        case TUNNEL_SOURCE_SR:
            return "sr";
        case TUNNEL_SOURCE_STATIC:
            return "static";
        case TUNNEL_SOURCE_CONNECTED:
            return "connected";
        default:
            return "unknown";
    }
}

static const char *action_name(uint8_t action)
{
    switch (action)
    {
        case TUNNEL_ACTION_DROP:
            return "drop";
        case TUNNEL_ACTION_PUSH:
            return "push";
        case TUNNEL_ACTION_SWAP:
            return "swap";
        case TUNNEL_ACTION_POP:
            return "pop";
        case TUNNEL_ACTION_PHP:
            return "php";
        default:
            return "unknown";
    }
}

static const char *afi_name(uint16_t afi)
{
    switch (afi)
    {
        case 1:
            return "ipv4";
        case 2:
            return "ipv6";
        default:
            return "unknown";
    }
}

static const char *module_name(uint32_t module_id)
{
    switch (module_id)
    {
        case DEV_MODULE_ID_DEV:
            return "dev";
        case DEV_MODULE_ID_DB:
            return "db";
        case DEV_MODULE_ID_CLI:
            return "cli";
        case DEV_MODULE_ID_IF:
            return "if";
        case DEV_MODULE_ID_BGP:
            return "bgp";
        case DEV_MODULE_ID_ROUTE:
            return "route";
        case DEV_MODULE_ID_VRF:
            return "vrf";
        case DEV_MODULE_ID_SBMP:
            return "sbmp";
        case DEV_MODULE_ID_ISIS:
            return "isis";
        case DEV_MODULE_ID_TUNNEL:
            return "tunnel";
        default:
            return "unknown";
    }
}

static void append_addr(GString *out, const net_addr_t *addr)
{
    char buf[64] = "-";
    if (addr && addr->family != 0)
    {
        net_addr_to_str(addr, buf, sizeof(buf));
    }
    g_string_append(out, buf);
}

static void append_labels(GString *out, uint8_t count, const uint32_t *labels)
{
    g_string_append_c(out, '[');
    for (uint8_t i = 0; i < count; i++)
    {
        if (i > 0)
        {
            g_string_append_c(out, ',');
        }
        g_string_append_printf(out, "%u", labels[i]);
    }
    g_string_append_c(out, ']');
}

char *tunnel_rib_show(const tunnel_rib_t *rib, tunnel_show_section_t section)
{
    GString *out = g_string_new(NULL);
    if (!out)
    {
        return NULL;
    }

    switch (section)
    {
        case TUNNEL_SHOW_CANDIDATE:
            g_string_append_printf(out, "Tunnel candidates: %u\r\n", rib ? g_list_length(rib->candidates) : 0);
            for (const GList *it = rib ? rib->candidates : NULL; it; it = it->next)
            {
                const tunnel_candidate_t *c = it->data;
                g_string_append_printf(out, "  vrf %u afi %u endpoint ", c->vrf_id, c->afi);
                append_addr(out, &c->endpoint);
                g_string_append_printf(out, " nh ");
                append_addr(out, &c->nexthop);
                g_string_append_printf(out, " relay ");
                append_addr(out, &c->relay_addr);
                g_string_append_printf(out, " oif %u src %s pref %u labels ", c->out_ifindex,
                                       source_name(c->source_type), c->preference);
                append_labels(out, c->label_count, c->labels);
                g_string_append(out, "\r\n");
            }
            break;

        case TUNNEL_SHOW_NHLFE:
            g_string_append_printf(out, "NHLFE: %u\r\n", rib ? g_list_length(rib->nhlfes) : 0);
            for (const GList *it = rib ? rib->nhlfes : NULL; it; it = it->next)
            {
                const tunnel_nhlfe_t *n = it->data;
                g_string_append_printf(out, "  id %u endpoint ", n->id);
                append_addr(out, &n->endpoint);
                g_string_append_printf(out, " relay ");
                append_addr(out, &n->relay_addr);
                g_string_append_printf(out, " oif %u src %s labels ", n->out_ifindex, source_name(n->source_type));
                append_labels(out, n->label_count, n->labels);
                g_string_append(out, "\r\n");
            }
            break;

        case TUNNEL_SHOW_FTN:
            g_string_append_printf(out, "FTN: %u\r\n", rib ? g_list_length(rib->ftns) : 0);
            for (const GList *it = rib ? rib->ftns : NULL; it; it = it->next)
            {
                const tunnel_ftn_t *f = it->data;
                g_string_append_printf(out, "  vrf %u afi %u fec ", f->fec.vrf_id, f->fec.afi);
                append_addr(out, &f->fec.addr);
                g_string_append_printf(out, "/%u -> nhlfe %u src %s state %s\r\n", f->fec.prefix_len, f->nhlfe_id,
                                       source_name(f->source_type), f->state ? "up" : "down");
            }
            break;

        case TUNNEL_SHOW_ILM:
            g_string_append_printf(out, "ILM: %u\r\n", rib ? g_list_length(rib->ilms) : 0);
            for (const GList *it = rib ? rib->ilms : NULL; it; it = it->next)
            {
                const tunnel_ilm_t *i = it->data;
                g_string_append_printf(out, "  vrf %u label %u -> nhlfe %u action %s(%u) state %s\r\n", i->vrf_id,
                                       i->in_label, i->nhlfe_id, action_name(i->action), i->action,
                                       i->state ? "up" : "down");
            }
            break;

        case TUNNEL_SHOW_WATCH:
            g_string_append_printf(out, "Resolve watches: %u\r\n", rib ? g_list_length(rib->watches) : 0);
            for (const GList *it = rib ? rib->watches : NULL; it; it = it->next)
            {
                const tunnel_watch_t *w = it->data;
                const tunnel_resolve_notify_t *n = &w->last_notify;
                g_string_append_printf(out, "  owner %s vrf %u afi %u endpoint ", module_name(w->owner_module_id),
                                       w->req.vrf_id, w->req.afi);
                append_addr(out, &w->req.endpoint);
                if (w->last_resolved < 0)
                {
                    g_string_append(out, " state unknown\r\n");
                    continue;
                }
                g_string_append_printf(out, " state %s tunnel %u relay ", n->resolved ? "up" : "down", n->tunnel_id);
                append_addr(out, &n->relay_addr);
                g_string_append_printf(out, " oif %u labels ", n->out_ifindex);
                append_labels(out, n->label_count, n->labels);
                g_string_append(out, "\r\n");
            }
            break;

        case TUNNEL_SHOW_SUMMARY:
        default:
            g_string_append(out, "Tunnel Summary\r\n");
            g_string_append_printf(out, "  Candidates    : %u\r\n", rib ? g_list_length(rib->candidates) : 0);
            g_string_append_printf(out, "  NHLFE         : %u\r\n", rib ? g_list_length(rib->nhlfes) : 0);
            g_string_append_printf(out, "  FTN           : %u\r\n", rib ? g_list_length(rib->ftns) : 0);
            g_string_append_printf(out, "  ILM           : %u\r\n", rib ? g_list_length(rib->ilms) : 0);
            g_string_append_printf(out, "  ResolveWatch  : %u\r\n", rib ? g_list_length(rib->watches) : 0);
            break;
    }
    return g_string_free(out, FALSE);
}

char *tunnel_rib_show_labels(const tunnel_rib_t *rib)
{
    GString *out = g_string_new(NULL);
    if (!out)
    {
        return NULL;
    }

    uint32_t count = rib ? (uint32_t)g_list_length(rib->label_bindings) : 0u;
    g_string_append_printf(out,
                           "\r\nTunnel Label Allocations\r\n"
                           "Dynamic range: %u-%u, allocated: %u\r\n"
                           "%-8s %-6s %-4s %-24s %-14s %-12s\r\n"
                           "-------- ------ ---- ------------------------ -------------- ------------\r\n",
                           rib ? rib->label_dynamic_min : 0u, rib ? rib->label_dynamic_max : 0u, count, "Label", "VRF",
                           "AFI", "FEC", "Owner", "Source");

    for (const GList *it = rib ? rib->label_bindings : NULL; it; it = it->next)
    {
        const tunnel_label_binding_t *b = it->data;
        if (!b)
        {
            continue;
        }

        char fec_addr[64] = "-";
        char fec[96];
        if (b->req.fec.addr.family != 0)
        {
            net_addr_to_str(&b->req.fec.addr, fec_addr, sizeof(fec_addr));
        }
        snprintf(fec, sizeof(fec), "%s/%u", fec_addr, b->req.fec.prefix_len);

        char owner[48];
        snprintf(owner, sizeof(owner), "%s:%u", module_name(b->req.owner_module_id), b->req.owner_id);

        g_string_append_printf(out, "%-8u %-6u %-4s %-24s %-14s %-12s\r\n", b->label, b->req.vrf_id,
                               afi_name(b->req.afi), fec, owner, source_name(b->req.source_type));
    }

    if (count == 0)
    {
        g_string_append(out, "  (no label allocations)\r\n");
    }
    g_string_append_printf(out, "\r\nTotal %u label allocation(s)\r\n", count);
    return g_string_free(out, FALSE);
}
