#include "rpm_main.h"

#include <arpa/inet.h>
#include <string.h>

#include "cli.h"
#include "db.h"
#include "errcode.h"
#include "log.h"
#include "rpm_bdr.h"
#include "rpm_cli.h"
#include "rpm_db.h"

#define RPM_MSG_TYPE_INTERNAL_DB_READY DEV_IPC_MSG_TYPE(DEV_IPC_CATEGORY_RPM, 0x1001)

rpm_local_t *g_rpm_local;

dev_ipc_context_t *rpm_local_ipc_ctx(void)
{
    return g_rpm_local ? g_rpm_local->dev_ipc_ctx : NULL;
}

const rpm_policy_t *rpm_policy_lookup(const char *name)
{
    return g_rpm_local && g_rpm_local->policies && name ? g_hash_table_lookup(g_rpm_local->policies, name) : NULL;
}

void rpm_policy_store(const rpm_policy_t *policy)
{
    if (!g_rpm_local || !g_rpm_local->policies || !policy || policy->name[0] == '\0')
    {
        return;
    }
    rpm_policy_t *copy = g_memdup2(policy, sizeof(*policy));
    g_mutex_lock(&g_rpm_local->lock);
    g_hash_table_replace(g_rpm_local->policies, g_strdup(policy->name), copy);
    g_mutex_unlock(&g_rpm_local->lock);
}

void rpm_policy_remove(const char *name)
{
    if (!g_rpm_local || !g_rpm_local->policies || !name)
    {
        return;
    }
    g_mutex_lock(&g_rpm_local->lock);
    g_hash_table_remove(g_rpm_local->policies, name);
    g_mutex_unlock(&g_rpm_local->lock);
}

static void rpm_send_ack(const dev_ipc_message_t *msg)
{
    dev_ipc_message_t *resp =
        dev_ipc_message_create(RPM_MSG_TYPE_ACK, DEV_MODULE_ID_RPM, msg->src_module_id, msg->request_id, NULL, 0, NULL);
    if (resp)
    {
        dev_ipc_send_response(rpm_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
}

static void rpm_send_policy_event(uint32_t module_id, uint32_t event, uint32_t object_mask, const rpm_policy_t *policy)
{
    rpm_policy_event_t *payload = g_new0(rpm_policy_event_t, 1);
    payload->event = event;
    payload->object_mask = object_mask;
    if (policy)
    {
        payload->policy = *policy;
    }
    dev_ipc_message_t *msg = dev_ipc_message_create(RPM_MSG_TYPE_POLICY_EVENT, DEV_MODULE_ID_RPM, module_id, 0, payload,
                                                    sizeof(*payload), g_free);
    if (msg)
    {
        (void)dev_ipc_send(rpm_local_ipc_ctx(), module_id, msg);
        dev_ipc_message_free(msg);
    }
}

void rpm_policy_publish(uint32_t event, const rpm_policy_t *policy)
{
    if (!g_rpm_local || !policy)
    {
        return;
    }
    GArray *targets = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    g_mutex_lock(&g_rpm_local->lock);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, g_rpm_local->subscribers);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        uint32_t mask = GPOINTER_TO_UINT(value);
        if ((mask & RPM_OBJECT_ROUTE_POLICY) != 0u)
        {
            uint32_t id = GPOINTER_TO_UINT(key);
            g_array_append_val(targets, id);
        }
    }
    g_mutex_unlock(&g_rpm_local->lock);
    for (guint i = 0; i < targets->len; i++)
    {
        rpm_send_policy_event(g_array_index(targets, uint32_t, i), event, RPM_OBJECT_ROUTE_POLICY, policy);
    }
    g_array_free(targets, TRUE);
}

void rpm_policy_publish_all_to(uint32_t module_id, uint32_t interest_mask)
{
    GPtrArray *snapshot = g_ptr_array_new_with_free_func(g_free);
    if ((interest_mask & RPM_OBJECT_ROUTE_POLICY) != 0u)
    {
        g_mutex_lock(&g_rpm_local->lock);
        GHashTableIter iter;
        gpointer value;
        g_hash_table_iter_init(&iter, g_rpm_local->policies);
        while (g_hash_table_iter_next(&iter, NULL, &value))
        {
            const rpm_policy_t *policy = value;
            g_ptr_array_add(snapshot, g_memdup2(policy, sizeof(*policy)));
        }
        g_mutex_unlock(&g_rpm_local->lock);
    }
    for (guint i = 0; i < snapshot->len; i++)
    {
        rpm_send_policy_event(module_id, RPM_POLICY_EVENT_UPSERT, RPM_OBJECT_ROUTE_POLICY,
                              g_ptr_array_index(snapshot, i));
    }
    rpm_send_policy_event(module_id, RPM_POLICY_EVENT_SMOOTH_END, interest_mask, NULL);
    g_ptr_array_free(snapshot, TRUE);
}

static void rpm_handle_subscribe(const dev_ipc_message_t *msg)
{
    if (!msg->payload || msg->payload_len < sizeof(rpm_subscribe_req_t))
    {
        rpm_send_ack(msg);
        return;
    }
    rpm_subscribe_req_t req;
    memcpy(&req, msg->payload, sizeof(req));
    uint32_t mask = ntohl(req.interest_mask);
    uint32_t flags = ntohl(req.flags);
    g_mutex_lock(&g_rpm_local->lock);
    g_hash_table_replace(g_rpm_local->subscribers, GUINT_TO_POINTER(msg->src_module_id), GUINT_TO_POINTER(mask));
    g_mutex_unlock(&g_rpm_local->lock);
    rpm_send_ack(msg);
    if ((flags & RPM_SUBSCRIBE_FLAG_REPLAY) != 0u)
    {
        rpm_policy_publish_all_to(msg->src_module_id, mask);
    }
}

static void rpm_handle_policy_get(const dev_ipc_message_t *msg)
{
    rpm_policy_get_resp_t *payload = g_new0(rpm_policy_get_resp_t, 1);
    if (msg->payload && msg->payload_len >= sizeof(rpm_policy_get_req_t))
    {
        rpm_policy_get_req_t req;
        memcpy(&req, msg->payload, sizeof(req));
        req.name[sizeof(req.name) - 1] = '\0';
        g_mutex_lock(&g_rpm_local->lock);
        const rpm_policy_t *policy = g_hash_table_lookup(g_rpm_local->policies, req.name);
        if (policy)
        {
            payload->found = 1u;
            payload->policy = *policy;
        }
        g_mutex_unlock(&g_rpm_local->lock);
    }
    dev_ipc_message_t *resp = dev_ipc_message_create(RPM_MSG_TYPE_POLICY_GET, DEV_MODULE_ID_RPM, msg->src_module_id,
                                                     msg->request_id, payload, sizeof(*payload), g_free);
    if (resp)
    {
        dev_ipc_send_response(rpm_local_ipc_ctx(), resp);
        dev_ipc_message_free(resp);
    }
    else
    {
        g_free(payload);
    }
}

static void rpm_on_disconnect(dev_ipc_context_t *ctx, uint32_t remote_module_id, void *user)
{
    (void)ctx;
    (void)user;
    if (!g_rpm_local)
    {
        return;
    }
    g_mutex_lock(&g_rpm_local->lock);
    g_hash_table_remove(g_rpm_local->subscribers, GUINT_TO_POINTER(remote_module_id));
    g_mutex_unlock(&g_rpm_local->lock);
}

static void rpm_handle_db_ready(void)
{
    dev_ipc_context_t *ctx = rpm_local_ipc_ctx();
    if (dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DB, DEV_IPC_WAIT_PEER_MS) != ERRCODE_SUCCESS)
    {
        LOG_WARN("RPM: DB connection not ready");
        return;
    }
    if (rpm_db_init() != ERRCODE_SUCCESS || rpm_db_restore() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("RPM: DB initialization/restore failed");
    }
}

static void rpm_on_db_event(uint32_t module_id, uint8_t event, const char *host, uint16_t port, uint32_t epoch,
                            void *user)
{
    (void)module_id;
    (void)host;
    (void)port;
    (void)epoch;
    (void)user;
    if (event == DEV_MODULE_EVENT_READY && g_rpm_local)
    {
        dev_ipc_message_t *msg = dev_ipc_message_create(RPM_MSG_TYPE_INTERNAL_DB_READY, DEV_MODULE_ID_RPM,
                                                        DEV_MODULE_ID_RPM, 0, NULL, 0, NULL);
        if (msg)
        {
            g_async_queue_push(g_rpm_local->dev_ipc_ctx->msg_queue, msg);
        }
    }
}

static void rpm_msg_handler(dev_ipc_context_t *ctx, dev_ipc_message_t *msg)
{
    if (!msg)
    {
        return;
    }
    switch (msg->msg_type)
    {
        case RPM_MSG_TYPE_INTERNAL_DB_READY:
            rpm_handle_db_ready();
            break;
        case RPM_MSG_TYPE_SUBSCRIBE:
            rpm_handle_subscribe(msg);
            break;
        case RPM_MSG_TYPE_UNSUBSCRIBE:
            g_mutex_lock(&g_rpm_local->lock);
            g_hash_table_remove(g_rpm_local->subscribers, GUINT_TO_POINTER(msg->src_module_id));
            g_mutex_unlock(&g_rpm_local->lock);
            rpm_send_ack(msg);
            break;
        case RPM_MSG_TYPE_POLICY_GET:
            rpm_handle_policy_get(msg);
            break;
        case CLI_MSG_TYPE:
            if (db_rpc_guard_reject(ctx, msg, "RPM"))
            {
                break;
            }
            (void)rpm_cli_handle_config_msg(msg);
            break;
        case CLI_MSG_TYPE_QUERY_CANDIDATES:
            rpm_cli_handle_candidates(msg);
            break;
        case CLI_MSG_TYPE_SHOW_CONFIG:
            (void)rpm_bdr_handle_show_config(msg);
            break;
        default:
            LOG_WARN("RPM: unknown message type 0x%08X", msg->msg_type);
            break;
    }
    dev_ipc_message_free(msg);
}

int rpm_module_init(void)
{
    log_set_tag("rpm");
    dev_ipc_context_t *ctx = dev_ipc_init(DEV_MODULE_ID_RPM, "rpm", DEV_MODULE_PORT_RPM, rpm_msg_handler);
    if (!ctx)
    {
        return -1;
    }
    g_rpm_local = g_new0(rpm_local_t, 1);
    g_rpm_local->dev_ipc_ctx = ctx;
    g_rpm_local->policies = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_rpm_local->subscribers = g_hash_table_new(g_direct_hash, g_direct_equal);
    g_mutex_init(&g_rpm_local->lock);
    dev_ipc_set_disconnect_handler(ctx, rpm_on_disconnect, NULL);

    (void)dev_ipc_wait_connected(ctx, DEV_MODULE_ID_DEV, DEV_IPC_WAIT_DEV_MS);
    if (dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_DB, 0, rpm_on_db_event, NULL) != ERRCODE_SUCCESS ||
        dev_ipc_subscribe_module(ctx, DEV_MODULE_ID_CLI, 0, NULL, NULL) != ERRCODE_SUCCESS)
    {
        LOG_WARN("RPM: failed to subscribe a dependency");
    }
    (void)dev_ipc_wait_all_subscribed_connected(ctx, 0);
    rpm_handle_db_ready();
    if (dev_ipc_notify_ready(ctx) != ERRCODE_SUCCESS)
    {
        LOG_WARN("RPM: notify_ready failed");
    }
    LOG_INFO("RPM: module ready");
    return 0;
}

void rpm_module_cleanup(void)
{
    if (!g_rpm_local)
    {
        return;
    }
    dev_ipc_context_t *ctx = g_rpm_local->dev_ipc_ctx;
    if (ctx)
    {
        (void)dev_ipc_pre_exit_notify(ctx, 3000);
        dev_ipc_destroy(ctx);
    }
    g_hash_table_destroy(g_rpm_local->policies);
    g_hash_table_destroy(g_rpm_local->subscribers);
    g_mutex_clear(&g_rpm_local->lock);
    g_free(g_rpm_local);
    g_rpm_local = NULL;
}
