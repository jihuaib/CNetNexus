#include "srv6_worker.h"

#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "errcode.h"
#include "fib.h"
#include "log.h"
#include "route.h"
#include "srv6.h"
#include "srv6_main.h"
#include "vrf.h"

#define SRV6_FIB_RPC_TIMEOUT_MS 3000u
#define SRV6_ROUTE_RPC_TIMEOUT_MS 3000u

_Static_assert((int)SRV6_BEHAVIOR_END_DT4 == (int)FIB_SRV6_BEHAVIOR_END_DT4, "End.DT4 behavior mismatch");
_Static_assert((int)SRV6_BEHAVIOR_END_DT6 == (int)FIB_SRV6_BEHAVIOR_END_DT6, "End.DT6 behavior mismatch");

typedef enum srv6_worker_cmd_type
{
    SRV6_WORKER_CMD_RPC = 1,
    SRV6_WORKER_CMD_SHOW,
    SRV6_WORKER_CMD_VRF_EVENT,
    SRV6_WORKER_CMD_VRF_DOWN,
    SRV6_WORKER_CMD_FIB_READY,
    SRV6_WORKER_CMD_FIB_DOWN,
    SRV6_WORKER_CMD_ROUTE_READY,
    SRV6_WORKER_CMD_ROUTE_DOWN,
    SRV6_WORKER_CMD_LOCATOR_UPSERT,
    SRV6_WORKER_CMD_LOCATOR_DELETE,
    SRV6_WORKER_CMD_DELETE_CONFIG,
    SRV6_WORKER_CMD_PREPARE_RESTORE,
    SRV6_WORKER_CMD_RESTORE,
    SRV6_WORKER_CMD_SHUTDOWN,
} srv6_worker_cmd_type_t;

typedef struct srv6_worker_cmd
{
    srv6_worker_cmd_type_t type;
    dev_ipc_message_t *msg;
    gboolean waitable;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    gboolean done;
    int rc;
    char error[256];
    union
    {
        srv6_locator_t locator;
        char locator_name[SRV6_LOCATOR_NAME_MAX];
        struct
        {
            const GPtrArray *locators;
            const GPtrArray *bindings;
        } restore;
    } u;
} srv6_worker_cmd_t;

srv6_work_local_t *g_srv6_work_local;

static int srv6_binding_release(srv6_binding_state_t *binding);

static guint srv6_sid_key_hash(gconstpointer data)
{
    const srv6_sid_key_t *key = data;
    guint hash = g_str_hash(key->locator);
    hash = hash * 33u + key->vrf_id;
    hash = hash * 33u + key->behavior;
    hash = hash * 33u + key->owner_module_id;
    hash = hash * 33u + key->owner_id;
    return hash;
}

static gboolean srv6_sid_key_equal(gconstpointer a, gconstpointer b)
{
    const srv6_sid_key_t *ka = a;
    const srv6_sid_key_t *kb = b;
    return ka->vrf_id == kb->vrf_id && ka->behavior == kb->behavior && ka->owner_module_id == kb->owner_module_id &&
           ka->owner_id == kb->owner_id && strcmp(ka->locator, kb->locator) == 0;
}

static void srv6_set_error(char *buf, size_t len, const char *text)
{
    if (buf && len > 0u)
    {
        g_strlcpy(buf, text ? text : "", len);
    }
}

static gboolean srv6_behavior_valid(uint16_t behavior)
{
    return behavior == SRV6_BEHAVIOR_END_DT4 || behavior == SRV6_BEHAVIOR_END_DT6;
}

static gboolean srv6_locator_name_valid(const char *name)
{
    if (!name || name[0] == '\0')
    {
        return FALSE;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean srv6_prefix_bits_equal(const struct in6_addr *a, const struct in6_addr *b, uint8_t bits)
{
    uint8_t whole = bits / 8u;
    uint8_t rem = bits % 8u;
    if (whole > 0u && memcmp(a->s6_addr, b->s6_addr, whole) != 0)
    {
        return FALSE;
    }
    if (rem > 0u)
    {
        uint8_t mask = (uint8_t)(0xFFu << (8u - rem));
        if ((a->s6_addr[whole] & mask) != (b->s6_addr[whole] & mask))
        {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean srv6_locators_overlap(const srv6_locator_t *a, const srv6_locator_t *b)
{
    uint8_t common = a->prefix_len < b->prefix_len ? a->prefix_len : b->prefix_len;
    return srv6_prefix_bits_equal(&a->prefix.u.v6, &b->prefix.u.v6, common);
}

static int srv6_locator_validate(const srv6_locator_t *input, srv6_locator_t *normalized, const char *exclude_name,
                                 char *error, size_t error_len)
{
    if (!input || !normalized || !srv6_locator_name_valid(input->name))
    {
        srv6_set_error(error, error_len, "invalid locator name");
        return ERRCODE_FAIL;
    }
    *normalized = *input;
    normalized->name[sizeof(normalized->name) - 1u] = '\0';
    if (normalized->prefix.family != AF_INET6 || normalized->prefix_len > 127u || normalized->function_bits == 0u ||
        normalized->function_bits > SRV6_FUNCTION_BITS_MAX ||
        (uint16_t)normalized->prefix_len + normalized->function_bits > 128u ||
        net_addr_prefix_normalize(&normalized->prefix, normalized->prefix_len) != 0)
    {
        srv6_set_error(error, error_len, "invalid IPv6 prefix or function-bits");
        return ERRCODE_FAIL;
    }

    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->locators);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const srv6_locator_t *other = value;
        if (exclude_name && strcmp(other->name, exclude_name) == 0)
        {
            continue;
        }
        if (srv6_locators_overlap(normalized, other))
        {
            srv6_set_error(error, error_len, "locator prefix overlaps an existing locator");
            return ERRCODE_FAIL;
        }
    }
    return ERRCODE_SUCCESS;
}

static gboolean srv6_locator_has_bindings(const char *name)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const srv6_binding_state_t *binding = value;
        if (strcmp(binding->entry.key.locator, name) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static void srv6_locator_fill_route(const srv6_locator_t *locator, route_msg_entry_t *route)
{
    memset(route, 0, sizeof(*route));
    route->vrf_id = ROUTE_VRF_DEFAULT;
    route->afi = ROUTE_AFI_IPV6;
    route->safi = ROUTE_SAFI_UNICAST;
    route->prefix_len = locator->prefix_len;
    route->protocol = ROUTE_PROTOCOL_SRV6;
    route->metric = 0;
    route->preference = ROUTE_ADMIN_DIST_SRV6;
    route->nh_type = ROUTE_NH_TYPE_BLACKHOLE;
    route->prefix_addr = locator->prefix;
    /* ROUTE 的 path key 是 protocol+source。locator 前缀不重叠，因此规范化前缀
     * 本身就是稳定且唯一的 owner key，更新/删除/进程重启都能命中同一路径。 */
    route->source_addr = locator->prefix;
    route->nexthop_addr.family = AF_INET6;
    g_strlcpy(route->source_name, locator->name, sizeof(route->source_name));
}

static int srv6_locator_route_program(const srv6_locator_t *locator)
{
    if (!locator || !g_srv6_work_local->route_ready)
    {
        return ERRCODE_DEP_MISSING;
    }
    route_msg_entry_t route;
    srv6_locator_fill_route(locator, &route);
    return route_rpc_add_wait(srv6_local_ipc_ctx(), &route, SRV6_ROUTE_RPC_TIMEOUT_MS);
}

static int srv6_locator_route_withdraw(const srv6_locator_t *locator)
{
    if (!locator || !g_srv6_work_local->route_ready)
    {
        return ERRCODE_DEP_MISSING;
    }
    route_msg_entry_t route;
    srv6_locator_fill_route(locator, &route);
    return route_rpc_del_wait(srv6_local_ipc_ctx(), &route, SRV6_ROUTE_RPC_TIMEOUT_MS);
}

static gboolean srv6_locator_route_key_equal(const srv6_locator_t *a, const srv6_locator_t *b)
{
    return a && b && a->prefix_len == b->prefix_len && net_addr_equal(&a->prefix, &b->prefix);
}

static int srv6_locator_routes_replay_all(void)
{
    if (!g_srv6_work_local->route_ready)
    {
        return ERRCODE_DEP_MISSING;
    }
    int result = ERRCODE_SUCCESS;
    GPtrArray *programmed = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->locators);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const srv6_locator_t *locator = value;
        int rc = srv6_locator_route_program(locator);
        if (rc != ERRCODE_SUCCESS)
        {
            char prefix[INET6_ADDRSTRLEN];
            net_addr_to_str(&locator->prefix, prefix, sizeof(prefix));
            LOG_WARN("SRV6: locator route replay failed name=%s prefix=%s/%u rc=%d", locator->name, prefix,
                     (unsigned)locator->prefix_len, rc);
            result = rc;
            break;
        }
        g_ptr_array_add(programmed, (gpointer)locator);
    }
    if (result != ERRCODE_SUCCESS)
    {
        for (guint i = 0u; i < programmed->len; ++i)
        {
            const srv6_locator_t *rollback = g_ptr_array_index(programmed, i);
            if (srv6_locator_route_withdraw(rollback) != ERRCODE_SUCCESS)
            {
                LOG_ERROR("SRV6: locator replay rollback could not withdraw route name=%s", rollback->name);
            }
        }
    }
    g_ptr_array_free(programmed, TRUE);
    return result;
}

static int srv6_locator_routes_flush_owned(void)
{
    if (!g_srv6_work_local->route_ready)
    {
        return ERRCODE_DEP_MISSING;
    }

    route_protocol_flush_req_t req;
    memset(&req, 0, sizeof(req));
    req.protocol = ROUTE_PROTOCOL_SRV6;
    req.vrf_id = ROUTE_VRF_DEFAULT;
    req.afi = ROUTE_AFI_IPV6;
    return route_rpc_flush_protocol_wait(srv6_local_ipc_ctx(), &req, SRV6_ROUTE_RPC_TIMEOUT_MS);
}

static gboolean srv6_function_used(const char *locator, uint32_t function_id)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const srv6_binding_state_t *binding = value;
        if (binding->entry.function_id == function_id && strcmp(binding->entry.key.locator, locator) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static uint32_t srv6_function_alloc(const srv6_locator_t *locator)
{
    uint64_t max_function = locator->function_bits == 32u ? UINT32_MAX : ((1ULL << locator->function_bits) - 1ULL);
    for (uint64_t candidate = 1u; candidate <= max_function; ++candidate)
    {
        if (!srv6_function_used(locator->name, (uint32_t)candidate))
        {
            return (uint32_t)candidate;
        }
    }
    return 0u;
}

static void srv6_sid_build(const srv6_locator_t *locator, uint32_t function_id, net_addr_t *sid)
{
    *sid = locator->prefix;
    for (uint8_t i = 0u; i < locator->function_bits; ++i)
    {
        uint8_t bit_pos = (uint8_t)(locator->prefix_len + i);
        uint8_t byte_idx = bit_pos / 8u;
        uint8_t mask = (uint8_t)(1u << (7u - (bit_pos % 8u)));
        uint8_t function_bit = (uint8_t)((function_id >> (locator->function_bits - 1u - i)) & 1u);
        if (function_bit)
        {
            sid->u.v6.s6_addr[byte_idx] |= mask;
        }
        else
        {
            sid->u.v6.s6_addr[byte_idx] &= (uint8_t)~mask;
        }
    }
}

static void srv6_fill_fib_entry(const srv6_sid_entry_t *entry, fib_srv6_localsid_entry_t *fib)
{
    memset(fib, 0, sizeof(*fib));
    fib->vrf_id = entry->key.vrf_id;
    fib->behavior = entry->key.behavior;
    fib->prefix_len = 128u;
    fib->sid = entry->sid;
}

static int srv6_binding_program(srv6_binding_state_t *binding)
{
    if (binding && binding->fib_programmed)
    {
        return ERRCODE_SUCCESS;
    }
    if (!binding || !g_srv6_work_local->fib_ready || !g_srv6_work_local->vrf_ready)
    {
        return ERRCODE_DEP_MISSING;
    }
    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup(binding->entry.key.vrf_id);
    if (!vrf || vrf->os_state != VRF_OS_STATE_UP)
    {
        binding->fib_programmed = FALSE;
        binding->last_error = ERRCODE_DEP_MISSING;
        return ERRCODE_DEP_MISSING;
    }

    fib_srv6_localsid_entry_t fib;
    srv6_fill_fib_entry(&binding->entry, &fib);
    int rc = fib_rpc_srv6_localsid_upsert_wait(srv6_local_ipc_ctx(), &fib, SRV6_FIB_RPC_TIMEOUT_MS);
    binding->fib_programmed = rc == ERRCODE_SUCCESS;
    binding->last_error = rc;
    return rc;
}

static int srv6_binding_unprogram(srv6_binding_state_t *binding)
{
    if (!binding || !g_srv6_work_local->fib_ready)
    {
        return ERRCODE_DEP_MISSING;
    }
    fib_srv6_localsid_entry_t fib;
    srv6_fill_fib_entry(&binding->entry, &fib);
    int rc = fib_rpc_srv6_localsid_delete_wait(srv6_local_ipc_ctx(), &fib, SRV6_FIB_RPC_TIMEOUT_MS);
    if (rc == ERRCODE_SUCCESS)
    {
        binding->fib_programmed = FALSE;
    }
    binding->last_error = rc;
    return rc;
}

static void srv6_binding_mark_all_unprogrammed(void)
{
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        srv6_binding_state_t *binding = value;
        binding->fib_programmed = FALSE;
        binding->last_error = ERRCODE_DEP_MISSING;
    }
}

static void srv6_binding_replay_all(void)
{
    if (!g_srv6_work_local->fib_ready || !g_srv6_work_local->vrf_ready)
    {
        return;
    }
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        srv6_binding_state_t *binding = value;
        int rc = srv6_binding_program(binding);
        if (rc != ERRCODE_SUCCESS)
        {
            char sid[INET6_ADDRSTRLEN];
            net_addr_to_str(&binding->entry.sid, sid, sizeof(sid));
            LOG_WARN("SRV6: localsid replay pending sid=%s vrf=%u rc=%d", sid, binding->entry.key.vrf_id, rc);
        }
    }
}

static void srv6_send_sid_result(const dev_ipc_message_t *req, int result, gboolean found,
                                 const srv6_sid_entry_t *entry)
{
    if (!req || req->request_id == 0u)
    {
        return;
    }
    srv6_sid_result_t *payload = g_new0(srv6_sid_result_t, 1);
    payload->result = result;
    payload->found = found ? 1u : 0u;
    if (entry)
    {
        payload->entry = *entry;
    }
    dev_ipc_message_t *resp = dev_ipc_message_create(SRV6_MSG_TYPE_SID_RESULT, DEV_MODULE_ID_SRV6, req->src_module_id,
                                                     req->request_id, payload, sizeof(*payload), g_free);
    if (!resp)
    {
        g_free(payload);
        return;
    }
    (void)dev_ipc_send_response(srv6_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

static void srv6_send_locator_result(const dev_ipc_message_t *req, int result, gboolean found)
{
    if (!req || req->request_id == 0u)
    {
        return;
    }
    srv6_locator_result_t *payload = g_new0(srv6_locator_result_t, 1);
    payload->result = result;
    payload->found = found ? 1u : 0u;
    dev_ipc_message_t *resp =
        dev_ipc_message_create(SRV6_MSG_TYPE_LOCATOR_RESULT, DEV_MODULE_ID_SRV6, req->src_module_id, req->request_id,
                               payload, sizeof(*payload), g_free);
    if (!resp)
    {
        g_free(payload);
        return;
    }
    (void)dev_ipc_send_response(srv6_local_ipc_ctx(), resp);
    dev_ipc_message_free(resp);
}

void srv6_worker_send_rpc_failure(const dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    if (msg->msg_type == SRV6_MSG_TYPE_LOCATOR_GET)
    {
        srv6_send_locator_result(msg, ERRCODE_FAIL, FALSE);
    }
    else if (msg->msg_type == SRV6_MSG_TYPE_SID_ALLOC || msg->msg_type == SRV6_MSG_TYPE_SID_RELEASE ||
             msg->msg_type == SRV6_MSG_TYPE_SID_RELEASE_OWNER || msg->msg_type == SRV6_MSG_TYPE_SID_GET)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
    }
}

static void srv6_handle_locator_get(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(srv6_locator_query_t))
    {
        srv6_send_locator_result(msg, ERRCODE_FAIL, FALSE);
        return;
    }
    srv6_locator_query_t query;
    memcpy(&query, msg->payload, sizeof(query));
    query.name[sizeof(query.name) - 1u] = '\0';
    if (!srv6_locator_name_valid(query.name))
    {
        srv6_send_locator_result(msg, ERRCODE_FAIL, FALSE);
        return;
    }
    srv6_send_locator_result(msg, ERRCODE_SUCCESS, g_hash_table_contains(g_srv6_work_local->locators, query.name));
}

static int srv6_key_from_request(const dev_ipc_message_t *msg, srv6_sid_key_t *key)
{
    if (!msg || !key || !msg->payload || msg->payload_len < sizeof(*key))
    {
        return ERRCODE_FAIL;
    }
    memset(key, 0, sizeof(*key));
    memcpy(key, msg->payload, sizeof(*key));
    key->locator[sizeof(key->locator) - 1u] = '\0';
    char locator[SRV6_LOCATOR_NAME_MAX];
    g_strlcpy(locator, key->locator, sizeof(locator));
    memset(key->locator, 0, sizeof(key->locator));
    g_strlcpy(key->locator, locator, sizeof(key->locator));
    key->_pad0 = 0u;
    key->owner_module_id = msg->src_module_id;
    if (!srv6_locator_name_valid(key->locator) || !srv6_behavior_valid(key->behavior) || key->owner_module_id == 0u ||
        key->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

static void srv6_handle_sid_alloc(dev_ipc_message_t *msg)
{
    srv6_sid_key_t key;
    if (srv6_key_from_request(msg, &key) != ERRCODE_SUCCESS)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }

    if (!g_srv6_work_local->route_ready)
    {
        srv6_send_sid_result(msg, ERRCODE_DEP_MISSING, FALSE, NULL);
        return;
    }

    srv6_binding_state_t *binding = g_hash_table_lookup(g_srv6_work_local->bindings, &key);
    if (binding)
    {
        int rc = srv6_binding_program(binding);
        srv6_send_sid_result(msg, rc, rc == ERRCODE_SUCCESS, rc == ERRCODE_SUCCESS ? &binding->entry : NULL);
        return;
    }

    if (!dev_ipc_is_connected(srv6_local_ipc_ctx(), DEV_MODULE_ID_DB) || !g_srv6_work_local->fib_ready ||
        !g_srv6_work_local->route_ready || !g_srv6_work_local->vrf_ready)
    {
        srv6_send_sid_result(msg, ERRCODE_DEP_MISSING, FALSE, NULL);
        return;
    }
    const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup(key.vrf_id);
    const srv6_locator_t *locator = g_hash_table_lookup(g_srv6_work_local->locators, key.locator);
    if (!locator || !vrf || vrf->os_state != VRF_OS_STATE_UP)
    {
        srv6_send_sid_result(msg, ERRCODE_DEP_MISSING, FALSE, NULL);
        return;
    }

    /* A service owner has exactly one SID per VRF/behavior/owner-id.  This
     * scope invariant lets an alloc repair an earlier locator-switch timeout:
     * a persisted binding whose exact locator key was lost is removed before
     * the requested incarnation is created. */
    GHashTableIter stale_iter;
    gpointer stale_value = NULL;
    g_hash_table_iter_init(&stale_iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&stale_iter, NULL, &stale_value))
    {
        srv6_binding_state_t *stale = stale_value;
        if (!stale || stale->entry.key.vrf_id != key.vrf_id || stale->entry.key.behavior != key.behavior ||
            stale->entry.key.owner_module_id != key.owner_module_id || stale->entry.key.owner_id != key.owner_id)
        {
            continue;
        }
        int cleanup_rc = srv6_binding_release(stale);
        if (cleanup_rc != ERRCODE_SUCCESS)
        {
            srv6_send_sid_result(msg, cleanup_rc, FALSE, NULL);
            return;
        }
        g_hash_table_iter_remove(&stale_iter);
    }

    uint32_t function_id = srv6_function_alloc(locator);
    if (function_id == 0u)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }

    srv6_binding_state_t *created = g_new0(srv6_binding_state_t, 1);
    created->entry.key = key;
    created->entry.function_id = function_id;
    created->entry.prefix_len = 128u;
    srv6_sid_build(locator, function_id, &created->entry.sid);
    if (srv6_db_sid_insert(&created->entry) != ERRCODE_SUCCESS)
    {
        g_free(created);
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }

    srv6_sid_key_t *stored_key = g_memdup2(&key, sizeof(key));
    if (!stored_key)
    {
        (void)srv6_db_sid_delete(&created->entry.key);
        g_free(created);
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }
    g_hash_table_insert(g_srv6_work_local->bindings, stored_key, created);

    int rc = srv6_binding_program(created);
    /* FIB 失败时保留持久绑定；下次重试仍使用同一个 SID，但本次不向调用方返回可通告 SID。 */
    srv6_send_sid_result(msg, rc, rc == ERRCODE_SUCCESS, rc == ERRCODE_SUCCESS ? &created->entry : NULL);
}

static void srv6_handle_sid_get(dev_ipc_message_t *msg)
{
    srv6_sid_key_t key;
    if (srv6_key_from_request(msg, &key) != ERRCODE_SUCCESS)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }
    if (!g_srv6_work_local->route_ready)
    {
        srv6_send_sid_result(msg, ERRCODE_DEP_MISSING, FALSE, NULL);
        return;
    }
    srv6_binding_state_t *binding = g_hash_table_lookup(g_srv6_work_local->bindings, &key);
    if (!binding)
    {
        srv6_send_sid_result(msg, ERRCODE_DEP_MISSING, FALSE, NULL);
        return;
    }
    int rc = srv6_binding_program(binding);
    srv6_send_sid_result(msg, rc, rc == ERRCODE_SUCCESS, rc == ERRCODE_SUCCESS ? &binding->entry : NULL);
}

static int srv6_binding_release(srv6_binding_state_t *binding)
{
    if (!binding)
    {
        return ERRCODE_SUCCESS;
    }
    int rc = srv6_binding_unprogram(binding);
    if (rc == ERRCODE_SUCCESS)
    {
        rc = srv6_db_sid_delete(&binding->entry.key);
        if (rc != ERRCODE_SUCCESS)
        {
            /* DB 仍声明 binding 存在，release 不能提交；把刚撤掉的
             * LocalSID 恢复，维持持久状态与 dataplane 一致。 */
            int restore_rc = srv6_binding_program(binding);
            if (restore_rc != ERRCODE_SUCCESS)
            {
                LOG_ERROR("SRV6: SID release DB rollback reprogram failed vrf=%u behavior=%s rc=%d",
                          binding->entry.key.vrf_id, srv6_behavior_name(binding->entry.key.behavior), restore_rc);
            }
        }
    }
    return rc;
}

static void srv6_handle_sid_release(dev_ipc_message_t *msg)
{
    srv6_sid_key_t key;
    if (srv6_key_from_request(msg, &key) != ERRCODE_SUCCESS)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }
    srv6_binding_state_t *binding = g_hash_table_lookup(g_srv6_work_local->bindings, &key);
    if (!binding)
    {
        srv6_send_sid_result(msg, ERRCODE_SUCCESS, FALSE, NULL);
        return;
    }
    int rc = srv6_binding_release(binding);
    if (rc == ERRCODE_SUCCESS)
    {
        g_hash_table_remove(g_srv6_work_local->bindings, &key);
    }
    srv6_send_sid_result(msg, rc, FALSE, NULL);
}

static void srv6_handle_sid_release_owner(dev_ipc_message_t *msg)
{
    if (!msg || !msg->payload || msg->payload_len < sizeof(srv6_sid_owner_scope_t))
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }
    srv6_sid_owner_scope_t scope;
    memcpy(&scope, msg->payload, sizeof(scope));
    scope._pad0 = 0u;
    if (scope.vrf_id == VRF_PUBLIC_VRF_ID || !srv6_behavior_valid(scope.behavior) || scope.owner_id == 0u ||
        msg->src_module_id == 0u)
    {
        srv6_send_sid_result(msg, ERRCODE_FAIL, FALSE, NULL);
        return;
    }

    int rc = ERRCODE_SUCCESS;
    gboolean found = FALSE;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        srv6_binding_state_t *binding = value;
        if (!binding || binding->entry.key.owner_module_id != msg->src_module_id ||
            binding->entry.key.owner_id != scope.owner_id || binding->entry.key.vrf_id != scope.vrf_id ||
            binding->entry.key.behavior != scope.behavior)
        {
            continue;
        }
        found = TRUE;
        if (srv6_binding_release(binding) != ERRCODE_SUCCESS)
        {
            rc = ERRCODE_FAIL;
            continue;
        }
        g_hash_table_iter_remove(&iter);
    }
    srv6_send_sid_result(msg, rc, found, NULL);
}

static int srv6_locator_sort(gconstpointer a, gconstpointer b)
{
    const srv6_locator_t *const *la = a;
    const srv6_locator_t *const *lb = b;
    return strcmp((*la)->name, (*lb)->name);
}

static int srv6_binding_sort(gconstpointer a, gconstpointer b)
{
    const srv6_binding_state_t *const *ba = a;
    const srv6_binding_state_t *const *bb = b;
    int rc = strcmp((*ba)->entry.key.locator, (*bb)->entry.key.locator);
    if (rc != 0)
    {
        return rc;
    }
    return net_addr_cmp(&(*ba)->entry.sid, &(*bb)->entry.sid);
}

static guint srv6_locator_binding_count(const char *name)
{
    guint count = 0u;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        const srv6_binding_state_t *binding = value;
        if (strcmp(binding->entry.key.locator, name) == 0)
        {
            ++count;
        }
    }
    return count;
}

static void srv6_show_append_locators(GString *out)
{
    GPtrArray *items = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->locators);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        g_ptr_array_add(items, value);
    }
    g_ptr_array_sort(items, srv6_locator_sort);
    g_string_append(out, "SRv6 Locators\r\n");
    g_string_append(out,
                    "Name                             Prefix                                    Func-bits  SIDs\r\n");
    for (guint i = 0; i < items->len; ++i)
    {
        const srv6_locator_t *locator = g_ptr_array_index(items, i);
        char prefix[INET6_ADDRSTRLEN];
        net_addr_to_str(&locator->prefix, prefix, sizeof(prefix));
        g_string_append_printf(out, "%-32s %-40s/%-3u %-10u %u\r\n", locator->name, prefix,
                               (unsigned)locator->prefix_len, (unsigned)locator->function_bits,
                               srv6_locator_binding_count(locator->name));
    }
    if (items->len == 0u)
    {
        g_string_append(out, "<none>\r\n");
    }
    g_ptr_array_free(items, TRUE);
}

static void srv6_show_append_bindings(GString *out)
{
    GPtrArray *items = g_ptr_array_new();
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        g_ptr_array_add(items, value);
    }
    g_ptr_array_sort(items, srv6_binding_sort);
    g_string_append(out, "SRv6 Local Service SIDs\r\n");
    g_string_append(
        out,
        "SID                                      Locator          VRF       Behavior Owner       Function State\r\n");
    for (guint i = 0; i < items->len; ++i)
    {
        const srv6_binding_state_t *binding = g_ptr_array_index(items, i);
        char sid[INET6_ADDRSTRLEN];
        net_addr_to_str(&binding->entry.sid, sid, sizeof(sid));
        char owner[32];
        snprintf(owner, sizeof(owner), "%u:%u", binding->entry.key.owner_module_id, binding->entry.key.owner_id);
        g_string_append_printf(out, "%-40s %-16s %-9u %-8s %-11s %-8u %s\r\n", sid, binding->entry.key.locator,
                               binding->entry.key.vrf_id, srv6_behavior_name(binding->entry.key.behavior), owner,
                               binding->entry.function_id, binding->fib_programmed ? "installed" : "pending");
    }
    if (items->len == 0u)
    {
        g_string_append(out, "<none>\r\n");
    }
    g_ptr_array_free(items, TRUE);
}

static void srv6_handle_show(dev_ipc_message_t *msg)
{
    if (msg->msg_type == CLI_MSG_TYPE_CONTINUE)
    {
        (void)cli_chunk_stream_continue(&g_srv6_work_local->show_stream, srv6_local_ipc_ctx(), DEV_MODULE_ID_SRV6, msg);
        return;
    }

    gboolean show_locator = FALSE;
    gboolean show_sid = FALSE;
    cli_tlv_parser_t parser;
    if (cli_tlv_init(&parser, msg->payload, msg->payload_len) == 0)
    {
        cli_tlv_entry_t entry;
        while (cli_tlv_next(&parser, &entry) == 1)
        {
            if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == 1u)
            {
                show_locator = TRUE;
            }
            else if (!CLI_TLV_IS_CTX(&entry) && entry.cfg_id == 2u)
            {
                show_sid = TRUE;
            }
            cli_tlv_entry_free(&entry);
        }
        cli_tlv_cleanup(&parser);
    }
    if (!show_locator && !show_sid)
    {
        show_locator = TRUE;
        show_sid = TRUE;
    }

    GString *out = g_string_new(NULL);
    if (show_locator)
    {
        srv6_show_append_locators(out);
    }
    if (show_locator && show_sid)
    {
        g_string_append(out, "\r\n");
    }
    if (show_sid)
    {
        srv6_show_append_bindings(out);
    }
    (void)cli_chunk_stream_start(&g_srv6_work_local->show_stream, srv6_local_ipc_ctx(), DEV_MODULE_ID_SRV6, msg, out);
}

static int srv6_worker_do_locator_upsert(const srv6_locator_t *locator, char *error, size_t error_len)
{
    srv6_locator_t normalized;
    if (srv6_locator_validate(locator, &normalized, locator ? locator->name : NULL, error, error_len) !=
        ERRCODE_SUCCESS)
    {
        return ERRCODE_FAIL;
    }
    const srv6_locator_t *old = g_hash_table_lookup(g_srv6_work_local->locators, normalized.name);
    srv6_locator_t old_copy;
    gboolean had_old = old != NULL;
    if (had_old)
    {
        old_copy = *old;
    }
    if (old && srv6_locator_has_bindings(normalized.name) &&
        (old->prefix_len != normalized.prefix_len || old->function_bits != normalized.function_bits ||
         !net_addr_equal(&old->prefix, &normalized.prefix)))
    {
        srv6_set_error(error, error_len, "locator has allocated SIDs and cannot be changed");
        return ERRCODE_FAIL;
    }

    if (!g_srv6_work_local->route_ready)
    {
        srv6_set_error(error, error_len, "ROUTE module is not ready");
        return ERRCODE_DEP_MISSING;
    }
    if (!g_srv6_work_local->fib_ready)
    {
        srv6_set_error(error, error_len, "FIB module is not ready");
        return ERRCODE_DEP_MISSING;
    }

    gboolean route_changed = !had_old || !srv6_locator_route_key_equal(&old_copy, &normalized);
    if (srv6_locator_route_program(&normalized) != ERRCODE_SUCCESS)
    {
        srv6_set_error(error, error_len, "failed to install locator route");
        return ERRCODE_FAIL;
    }
    if (had_old && route_changed && srv6_locator_route_withdraw(&old_copy) != ERRCODE_SUCCESS)
    {
        /* ROUTE may have accepted the new locator before rejecting the old
         * withdraw.  Re-advertise the still-authoritative old locator first,
         * so name-based consumers (ISIS) converge back to the persisted
         * prefix before the speculative new route is withdrawn. */
        if (srv6_locator_route_program(&old_copy) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("SRV6: locator update rollback could not restore old route name=%s", old_copy.name);
        }
        if (srv6_locator_route_withdraw(&normalized) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("SRV6: locator update rollback could not withdraw new route name=%s", normalized.name);
        }
        srv6_set_error(error, error_len, "failed to withdraw previous locator route");
        return ERRCODE_FAIL;
    }

    if (srv6_db_locator_upsert(&normalized) != ERRCODE_SUCCESS)
    {
        if (had_old && route_changed && srv6_locator_route_program(&old_copy) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("SRV6: locator DB rollback could not restore old route name=%s", old_copy.name);
        }
        if (route_changed && srv6_locator_route_withdraw(&normalized) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("SRV6: locator DB rollback could not withdraw new route name=%s", normalized.name);
        }
        srv6_set_error(error, error_len, "failed to persist locator");
        return ERRCODE_FAIL;
    }
    g_hash_table_replace(g_srv6_work_local->locators, g_strdup(normalized.name),
                         g_memdup2(&normalized, sizeof(normalized)));
    return ERRCODE_SUCCESS;
}

static int srv6_worker_do_locator_delete(const char *name, char *error, size_t error_len)
{
    if (!srv6_locator_name_valid(name))
    {
        srv6_set_error(error, error_len, "invalid locator name");
        return ERRCODE_FAIL;
    }
    if (srv6_locator_has_bindings(name))
    {
        srv6_set_error(error, error_len, "locator still has allocated SIDs");
        return ERRCODE_FAIL;
    }

    const srv6_locator_t *current = g_hash_table_lookup(g_srv6_work_local->locators, name);
    srv6_locator_t current_copy;
    if (current)
    {
        current_copy = *current;
        if (!g_srv6_work_local->route_ready)
        {
            srv6_set_error(error, error_len, "ROUTE module is not ready");
            return ERRCODE_DEP_MISSING;
        }
        if (!g_srv6_work_local->fib_ready)
        {
            srv6_set_error(error, error_len, "FIB module is not ready");
            return ERRCODE_DEP_MISSING;
        }
        if (srv6_locator_route_withdraw(&current_copy) != ERRCODE_SUCCESS)
        {
            srv6_set_error(error, error_len, "failed to withdraw locator route");
            return ERRCODE_FAIL;
        }
    }
    if (srv6_db_locator_delete(name) != ERRCODE_SUCCESS)
    {
        if (current && srv6_locator_route_program(&current_copy) != ERRCODE_SUCCESS)
        {
            LOG_ERROR("SRV6: locator delete DB rollback could not restore route name=%s", name);
        }
        srv6_set_error(error, error_len, "failed to delete persisted locator");
        return ERRCODE_FAIL;
    }
    g_hash_table_remove(g_srv6_work_local->locators, name);
    return ERRCODE_SUCCESS;
}

static int srv6_worker_do_delete_config(char *error, size_t error_len)
{
    if (g_hash_table_size(g_srv6_work_local->bindings) > 0u)
    {
        srv6_set_error(error, error_len, "active SID bindings exist; remove the owning service configuration first");
        return ERRCODE_FAIL;
    }

    GPtrArray *locators = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, g_srv6_work_local->locators);
    while (g_hash_table_iter_next(&iter, NULL, &value))
    {
        g_ptr_array_add(locators, g_memdup2(value, sizeof(srv6_locator_t)));
    }
    if (locators->len > 0u && !g_srv6_work_local->route_ready)
    {
        g_ptr_array_free(locators, TRUE);
        srv6_set_error(error, error_len, "ROUTE module is not ready");
        return ERRCODE_DEP_MISSING;
    }
    if (locators->len > 0u && !g_srv6_work_local->fib_ready)
    {
        g_ptr_array_free(locators, TRUE);
        srv6_set_error(error, error_len, "FIB module is not ready");
        return ERRCODE_DEP_MISSING;
    }

    guint withdrawn = 0u;
    for (; withdrawn < locators->len; ++withdrawn)
    {
        const srv6_locator_t *locator = g_ptr_array_index(locators, withdrawn);
        if (srv6_locator_route_withdraw(locator) != ERRCODE_SUCCESS)
        {
            for (guint i = 0u; i < withdrawn; ++i)
            {
                const srv6_locator_t *rollback = g_ptr_array_index(locators, i);
                if (srv6_locator_route_program(rollback) != ERRCODE_SUCCESS)
                {
                    LOG_ERROR("SRV6: no-srv6 rollback could not restore locator route name=%s", rollback->name);
                }
            }
            g_ptr_array_free(locators, TRUE);
            srv6_set_error(error, error_len, "failed to withdraw all locator routes");
            return ERRCODE_FAIL;
        }
    }
    if (srv6_db_delete_config() != ERRCODE_SUCCESS)
    {
        for (guint i = 0u; i < locators->len; ++i)
        {
            const srv6_locator_t *rollback = g_ptr_array_index(locators, i);
            if (srv6_locator_route_program(rollback) != ERRCODE_SUCCESS)
            {
                LOG_ERROR("SRV6: no-srv6 DB rollback could not restore locator route name=%s", rollback->name);
            }
        }
        g_ptr_array_free(locators, TRUE);
        srv6_set_error(error, error_len, "failed to clear persisted SRv6 configuration");
        return ERRCODE_FAIL;
    }
    g_ptr_array_free(locators, TRUE);
    g_hash_table_remove_all(g_srv6_work_local->bindings);
    g_hash_table_remove_all(g_srv6_work_local->locators);
    return ERRCODE_SUCCESS;
}

static int srv6_worker_do_restore(const GPtrArray *locators, const GPtrArray *bindings)
{
    g_hash_table_remove_all(g_srv6_work_local->bindings);
    g_hash_table_remove_all(g_srv6_work_local->locators);

    for (guint i = 0; locators && i < locators->len; ++i)
    {
        const srv6_locator_t *source = g_ptr_array_index((GPtrArray *)locators, i);
        srv6_locator_t normalized;
        char error[128];
        if (srv6_locator_validate(source, &normalized, NULL, error, sizeof(error)) != ERRCODE_SUCCESS)
        {
            LOG_WARN("SRV6: skipping persisted locator %s: %s", source ? source->name : "", error);
            continue;
        }
        g_hash_table_insert(g_srv6_work_local->locators, g_strdup(normalized.name),
                            g_memdup2(&normalized, sizeof(normalized)));
    }

    if (!g_srv6_work_local->route_ready)
    {
        LOG_WARN("SRV6: cannot restore locator routes while ROUTE is unavailable");
        return ERRCODE_DEP_MISSING;
    }
    GPtrArray *programmed = g_ptr_array_new();
    GHashTableIter locator_iter;
    gpointer locator_value = NULL;
    g_hash_table_iter_init(&locator_iter, g_srv6_work_local->locators);
    while (g_hash_table_iter_next(&locator_iter, NULL, &locator_value))
    {
        const srv6_locator_t *locator = locator_value;
        if (srv6_locator_route_program(locator) != ERRCODE_SUCCESS)
        {
            char prefix[INET6_ADDRSTRLEN];
            net_addr_to_str(&locator->prefix, prefix, sizeof(prefix));
            LOG_ERROR("SRV6: persisted locator route restore failed name=%s prefix=%s/%u", locator->name, prefix,
                      (unsigned)locator->prefix_len);
            for (guint i = 0u; i < programmed->len; ++i)
            {
                const srv6_locator_t *rollback = g_ptr_array_index(programmed, i);
                if (srv6_locator_route_withdraw(rollback) != ERRCODE_SUCCESS)
                {
                    LOG_ERROR("SRV6: restore rollback could not withdraw locator route name=%s", rollback->name);
                }
            }
            g_ptr_array_free(programmed, TRUE);
            return ERRCODE_FAIL;
        }
        g_ptr_array_add(programmed, (gpointer)locator);
    }
    g_ptr_array_free(programmed, TRUE);

    for (guint i = 0; bindings && i < bindings->len; ++i)
    {
        const srv6_sid_entry_t *source = g_ptr_array_index((GPtrArray *)bindings, i);
        const srv6_locator_t *locator = g_hash_table_lookup(g_srv6_work_local->locators, source->key.locator);
        if (!locator || !srv6_behavior_valid(source->key.behavior) || source->key.owner_module_id == 0u ||
            source->function_id == 0u || srv6_function_used(source->key.locator, source->function_id))
        {
            LOG_WARN("SRV6: skipping invalid/orphan persisted SID binding locator=%s", source->key.locator);
            continue;
        }
        uint64_t max_function = locator->function_bits == 32u ? UINT32_MAX : ((1ULL << locator->function_bits) - 1ULL);
        if ((uint64_t)source->function_id > max_function)
        {
            LOG_WARN("SRV6: skipping out-of-range SID function locator=%s function=%u", source->key.locator,
                     source->function_id);
            continue;
        }
        srv6_binding_state_t *state = g_new0(srv6_binding_state_t, 1);
        state->entry = *source;
        state->entry.key._pad0 = 0u;
        state->entry.prefix_len = 128u;
        /* function_id 是稳定分配真值；按当前 locator 规范重建并校验 SID。 */
        net_addr_t expected;
        srv6_sid_build(locator, source->function_id, &expected);
        if (!net_addr_equal(&expected, &source->sid))
        {
            char old_sid[INET6_ADDRSTRLEN], expected_sid[INET6_ADDRSTRLEN];
            net_addr_to_str(&source->sid, old_sid, sizeof(old_sid));
            net_addr_to_str(&expected, expected_sid, sizeof(expected_sid));
            LOG_WARN("SRV6: persisted SID mismatch locator=%s stored=%s expected=%s; binding ignored",
                     source->key.locator, old_sid, expected_sid);
            g_free(state);
            continue;
        }
        srv6_sid_key_t *stored_key = g_memdup2(&state->entry.key, sizeof(state->entry.key));
        if (!stored_key || g_hash_table_contains(g_srv6_work_local->bindings, stored_key))
        {
            g_free(stored_key);
            g_free(state);
            continue;
        }
        g_hash_table_insert(g_srv6_work_local->bindings, stored_key, state);
    }
    srv6_binding_replay_all();
    return ERRCODE_SUCCESS;
}

static srv6_worker_cmd_t *srv6_worker_cmd_new(srv6_worker_cmd_type_t type, gboolean waitable)
{
    srv6_worker_cmd_t *cmd = g_new0(srv6_worker_cmd_t, 1);
    cmd->type = type;
    cmd->waitable = waitable;
    if (waitable)
    {
        pthread_mutex_init(&cmd->mutex, NULL);
        pthread_cond_init(&cmd->cond, NULL);
    }
    return cmd;
}

static void srv6_worker_cmd_free(srv6_worker_cmd_t *cmd)
{
    if (!cmd)
    {
        return;
    }
    if (cmd->msg)
    {
        dev_ipc_message_free(cmd->msg);
    }
    if (cmd->waitable)
    {
        pthread_mutex_destroy(&cmd->mutex);
        pthread_cond_destroy(&cmd->cond);
    }
    g_free(cmd);
}

static void srv6_worker_cmd_complete(srv6_worker_cmd_t *cmd, int rc)
{
    pthread_mutex_lock(&cmd->mutex);
    cmd->rc = rc;
    cmd->done = TRUE;
    pthread_cond_signal(&cmd->cond);
    pthread_mutex_unlock(&cmd->mutex);
}

static int srv6_worker_cmd_wait(srv6_worker_cmd_t *cmd, char *error, size_t error_len)
{
    pthread_mutex_lock(&cmd->mutex);
    while (!cmd->done)
    {
        pthread_cond_wait(&cmd->cond, &cmd->mutex);
    }
    int rc = cmd->rc;
    if (error && error_len > 0u)
    {
        g_strlcpy(error, cmd->error, error_len);
    }
    pthread_mutex_unlock(&cmd->mutex);
    return rc;
}

static int srv6_worker_enqueue(srv6_worker_cmd_t *cmd)
{
    if (!cmd || !g_srv6_work_local || !g_srv6_work_local->cmd_queue || !g_srv6_work_local->running)
    {
        return ERRCODE_FAIL;
    }
    g_async_queue_push(g_srv6_work_local->cmd_queue, cmd);
    return ERRCODE_SUCCESS;
}

static void srv6_worker_handle_vrf_event(dev_ipc_message_t *msg)
{
    uint32_t event = 0u;
    uint32_t vrf_id = 0u;
    if (msg && msg->payload && msg->payload_len >= offsetof(vrf_event_msg_t, rts))
    {
        const vrf_event_msg_t *vrf_event = msg->payload;
        event = vrf_event->event;
        vrf_id = vrf_event->vrf_id;
    }
    vrf_api_cache_on_event(msg);
    if (event == VRF_EVENT_SMOOTHSTART)
    {
        g_srv6_work_local->vrf_ready = FALSE;
        srv6_binding_mark_all_unprogrammed();
    }
    else if (event == VRF_EVENT_SMOOTHEND)
    {
        g_srv6_work_local->vrf_ready = TRUE;
        srv6_binding_replay_all();
    }
    else if (event == VRF_EVENT_VRF_DEL || event == VRF_EVENT_VRF_STATE)
    {
        GHashTableIter iter;
        gpointer value = NULL;
        g_hash_table_iter_init(&iter, g_srv6_work_local->bindings);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            srv6_binding_state_t *binding = value;
            if (binding->entry.key.vrf_id == vrf_id)
            {
                const vrf_api_cache_entry_t *vrf = vrf_api_cache_lookup(vrf_id);
                if (!vrf || vrf->os_state != VRF_OS_STATE_UP)
                {
                    binding->fib_programmed = FALSE;
                    binding->last_error = ERRCODE_DEP_MISSING;
                }
                else
                {
                    (void)srv6_binding_program(binding);
                }
            }
        }
    }
}

static void srv6_worker_handle_rpc(dev_ipc_message_t *msg)
{
    switch (msg->msg_type)
    {
        case SRV6_MSG_TYPE_SID_ALLOC:
            srv6_handle_sid_alloc(msg);
            break;
        case SRV6_MSG_TYPE_SID_RELEASE:
            srv6_handle_sid_release(msg);
            break;
        case SRV6_MSG_TYPE_SID_RELEASE_OWNER:
            srv6_handle_sid_release_owner(msg);
            break;
        case SRV6_MSG_TYPE_SID_GET:
            srv6_handle_sid_get(msg);
            break;
        case SRV6_MSG_TYPE_LOCATOR_GET:
            srv6_handle_locator_get(msg);
            break;
        default:
            break;
    }
}

static void *srv6_worker_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "srv6-worker");
    log_set_tag("srv6");
    while (g_srv6_work_local->running)
    {
        srv6_worker_cmd_t *cmd = g_async_queue_pop(g_srv6_work_local->cmd_queue);
        gboolean shutting_down = cmd->type == SRV6_WORKER_CMD_SHUTDOWN;
        int rc = ERRCODE_SUCCESS;
        switch (cmd->type)
        {
            case SRV6_WORKER_CMD_RPC:
                srv6_worker_handle_rpc(cmd->msg);
                break;
            case SRV6_WORKER_CMD_SHOW:
                srv6_handle_show(cmd->msg);
                break;
            case SRV6_WORKER_CMD_VRF_EVENT:
                srv6_worker_handle_vrf_event(cmd->msg);
                break;
            case SRV6_WORKER_CMD_VRF_DOWN:
                vrf_api_cache_clear();
                g_srv6_work_local->vrf_ready = FALSE;
                srv6_binding_mark_all_unprogrammed();
                break;
            case SRV6_WORKER_CMD_FIB_READY:
                g_srv6_work_local->fib_ready = TRUE;
                srv6_binding_replay_all();
                break;
            case SRV6_WORKER_CMD_FIB_DOWN:
                g_srv6_work_local->fib_ready = FALSE;
                srv6_binding_mark_all_unprogrammed();
                break;
            case SRV6_WORKER_CMD_ROUTE_READY:
                g_srv6_work_local->route_ready = TRUE;
                rc = srv6_locator_routes_replay_all();
                if (rc != ERRCODE_SUCCESS)
                {
                    g_srv6_work_local->route_ready = FALSE;
                }
                break;
            case SRV6_WORKER_CMD_ROUTE_DOWN:
                g_srv6_work_local->route_ready = FALSE;
                break;
            case SRV6_WORKER_CMD_LOCATOR_UPSERT:
                rc = srv6_worker_do_locator_upsert(&cmd->u.locator, cmd->error, sizeof(cmd->error));
                break;
            case SRV6_WORKER_CMD_LOCATOR_DELETE:
                rc = srv6_worker_do_locator_delete(cmd->u.locator_name, cmd->error, sizeof(cmd->error));
                break;
            case SRV6_WORKER_CMD_DELETE_CONFIG:
                rc = srv6_worker_do_delete_config(cmd->error, sizeof(cmd->error));
                break;
            case SRV6_WORKER_CMD_PREPARE_RESTORE:
                rc = srv6_locator_routes_flush_owned();
                break;
            case SRV6_WORKER_CMD_RESTORE:
                rc = srv6_worker_do_restore(cmd->u.restore.locators, cmd->u.restore.bindings);
                break;
            case SRV6_WORKER_CMD_SHUTDOWN:
                /* SID 是持久配置，进程重启不能主动拆 dataplane；否则 BGP 在
                 * SRV6 模块重启窗口仍可能通告该 SID。显式 release/no srv6
                 * 才负责同步撤销 LocalSID，启动恢复会幂等 REPLACE。 */
                g_srv6_work_local->running = 0;
                break;
        }

        if (cmd->waitable)
        {
            srv6_worker_cmd_complete(cmd, rc);
        }
        else
        {
            srv6_worker_cmd_free(cmd);
        }
        if (shutting_down)
        {
            break;
        }
    }
    return NULL;
}

int srv6_worker_prepare(void)
{
    if (g_srv6_work_local)
    {
        return ERRCODE_SUCCESS;
    }
    g_srv6_work_local = g_new0(srv6_work_local_t, 1);
    g_srv6_work_local->locators = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_srv6_work_local->bindings = g_hash_table_new_full(srv6_sid_key_hash, srv6_sid_key_equal, g_free, g_free);
    g_srv6_work_local->cmd_queue = g_async_queue_new();
    if (!g_srv6_work_local->locators || !g_srv6_work_local->bindings || !g_srv6_work_local->cmd_queue)
    {
        return ERRCODE_FAIL;
    }
    vrf_api_cache_init();
    g_srv6_work_local->running = 1;
    return ERRCODE_SUCCESS;
}

int srv6_worker_launch(void)
{
    if (!g_srv6_work_local || g_srv6_work_local->thread_started)
    {
        return ERRCODE_FAIL;
    }
    if (pthread_create(&g_srv6_work_local->thread, NULL, srv6_worker_thread, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    g_srv6_work_local->thread_started = TRUE;
    return ERRCODE_SUCCESS;
}

static int srv6_worker_dispatch_wait(srv6_worker_cmd_t *cmd, char *error, size_t error_len);

static int srv6_worker_post_simple(srv6_worker_cmd_type_t type, dev_ipc_message_t *msg)
{
    srv6_worker_cmd_t *cmd = srv6_worker_cmd_new(type, FALSE);
    cmd->msg = msg;
    if (srv6_worker_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        cmd->msg = NULL;
        srv6_worker_cmd_free(cmd);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCCESS;
}

int srv6_worker_post_rpc(dev_ipc_message_t *msg)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_RPC, msg);
}

int srv6_worker_post_show(dev_ipc_message_t *msg)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_SHOW, msg);
}

int srv6_worker_post_vrf_event(dev_ipc_message_t *msg)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_VRF_EVENT, msg);
}

int srv6_worker_post_vrf_down(void)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_VRF_DOWN, NULL);
}

int srv6_worker_post_fib_ready(void)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_FIB_READY, NULL);
}

int srv6_worker_post_fib_down(void)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_FIB_DOWN, NULL);
}

int srv6_worker_post_route_ready(void)
{
    return srv6_worker_dispatch_wait(srv6_worker_cmd_new(SRV6_WORKER_CMD_ROUTE_READY, TRUE), NULL, 0u);
}

int srv6_worker_post_route_down(void)
{
    return srv6_worker_post_simple(SRV6_WORKER_CMD_ROUTE_DOWN, NULL);
}

int srv6_worker_prepare_restore(void)
{
    return srv6_worker_dispatch_wait(srv6_worker_cmd_new(SRV6_WORKER_CMD_PREPARE_RESTORE, TRUE), NULL, 0u);
}

static int srv6_worker_dispatch_wait(srv6_worker_cmd_t *cmd, char *error, size_t error_len)
{
    if (srv6_worker_enqueue(cmd) != ERRCODE_SUCCESS)
    {
        srv6_worker_cmd_free(cmd);
        return ERRCODE_FAIL;
    }
    int rc = srv6_worker_cmd_wait(cmd, error, error_len);
    srv6_worker_cmd_free(cmd);
    return rc;
}

int srv6_worker_locator_upsert(const srv6_locator_t *locator, char *error, size_t error_len)
{
    if (!locator)
    {
        return ERRCODE_FAIL;
    }
    srv6_worker_cmd_t *cmd = srv6_worker_cmd_new(SRV6_WORKER_CMD_LOCATOR_UPSERT, TRUE);
    cmd->u.locator = *locator;
    return srv6_worker_dispatch_wait(cmd, error, error_len);
}

int srv6_worker_locator_delete(const char *name, char *error, size_t error_len)
{
    if (!name)
    {
        return ERRCODE_FAIL;
    }
    srv6_worker_cmd_t *cmd = srv6_worker_cmd_new(SRV6_WORKER_CMD_LOCATOR_DELETE, TRUE);
    g_strlcpy(cmd->u.locator_name, name, sizeof(cmd->u.locator_name));
    return srv6_worker_dispatch_wait(cmd, error, error_len);
}

int srv6_worker_delete_config(char *error, size_t error_len)
{
    return srv6_worker_dispatch_wait(srv6_worker_cmd_new(SRV6_WORKER_CMD_DELETE_CONFIG, TRUE), error, error_len);
}

int srv6_worker_restore(const GPtrArray *locators, const GPtrArray *bindings)
{
    srv6_worker_cmd_t *cmd = srv6_worker_cmd_new(SRV6_WORKER_CMD_RESTORE, TRUE);
    cmd->u.restore.locators = locators;
    cmd->u.restore.bindings = bindings;
    return srv6_worker_dispatch_wait(cmd, NULL, 0u);
}

void srv6_worker_shutdown(void)
{
    if (!g_srv6_work_local)
    {
        return;
    }
    if (g_srv6_work_local->thread_started && g_srv6_work_local->running)
    {
        srv6_worker_cmd_t *cmd = srv6_worker_cmd_new(SRV6_WORKER_CMD_SHUTDOWN, FALSE);
        if (srv6_worker_enqueue(cmd) != ERRCODE_SUCCESS)
        {
            srv6_worker_cmd_free(cmd);
            g_srv6_work_local->running = 0;
        }
        pthread_join(g_srv6_work_local->thread, NULL);
    }
    cli_chunk_stream_reset(&g_srv6_work_local->show_stream);
    vrf_api_cache_cleanup();
    g_hash_table_destroy(g_srv6_work_local->bindings);
    g_hash_table_destroy(g_srv6_work_local->locators);
    while (g_async_queue_length(g_srv6_work_local->cmd_queue) > 0)
    {
        srv6_worker_cmd_t *cmd = g_async_queue_try_pop(g_srv6_work_local->cmd_queue);
        if (cmd)
        {
            srv6_worker_cmd_free(cmd);
        }
    }
    g_async_queue_unref(g_srv6_work_local->cmd_queue);
    g_free(g_srv6_work_local);
    g_srv6_work_local = NULL;
}
