/**
 * @file   ospf_route_sync.c
 * @brief  Ordered asynchronous synchronization of OSPF routes to ROUTE
 */
#include "ospf_route_sync.h"

#include <pthread.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "ospf_main.h"
#include "ospf_worker.h"
#include "route.h"

#define OSPF_ROUTE_SYNC_RPC_TIMEOUT_MS 500u
#define OSPF_ROUTE_SYNC_RETRY_MIN_MS 100u
#define OSPF_ROUTE_SYNC_RETRY_MAX_MS 1000u
#define OSPF_ROUTE_SYNC_SHUTDOWN_TIMEOUT_MS 3000u

typedef enum ospf_route_sync_op_type
{
    OSPF_ROUTE_SYNC_ADD = 1,
    OSPF_ROUTE_SYNC_DEL = 2,
} ospf_route_sync_op_type_t;

typedef struct ospf_route_sync_op
{
    ospf_route_sync_op_type_t type;
    route_msg_entry_t entry;
} ospf_route_sync_op_t;

typedef struct ospf_route_sync_job
{
    uint8_t op_count;
    ospf_route_sync_op_t ops[2];
} ospf_route_sync_job_t;

typedef struct ospf_route_sync_local
{
    GMutex mutex;
    GCond cond;
    GQueue *queue;
    pthread_t thread;
    gboolean accepting;
    gboolean stopping;
    gboolean thread_started;
    gint64 shutdown_deadline_usec;
} ospf_route_sync_local_t;

static ospf_route_sync_local_t *g_ospf_route_sync;

static void ospf_route_sync_fill_entry(const ospf_route_t *route, route_msg_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = route->vrf_id;
    entry->afi = ROUTE_AFI_IPV4;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = route->prefix_len;
    entry->protocol = ROUTE_PROTOCOL_OSPF;
    entry->metric = (int32_t)route->metric;
    entry->preference = ROUTE_ADMIN_DIST_OSPF;
    entry->nh_type = ROUTE_NH_TYPE_IP;
    entry->out_ifindex = route->out_ifindex;
    entry->prefix_addr = route->prefix;
    entry->source_addr = route->source;
    entry->nexthop_addr = route->nexthop;
}

static ospf_route_sync_job_t *ospf_route_sync_job_create(const ospf_route_t *first,
                                                         ospf_route_sync_op_type_t first_type,
                                                         const ospf_route_t *second,
                                                         ospf_route_sync_op_type_t second_type)
{
    if (!first)
    {
        return NULL;
    }

    ospf_route_sync_job_t *job = g_malloc0(sizeof(*job));
    if (!job)
    {
        return NULL;
    }

    job->op_count = second ? 2u : 1u;
    job->ops[0].type = first_type;
    ospf_route_sync_fill_entry(first, &job->ops[0].entry);
    if (second)
    {
        job->ops[1].type = second_type;
        ospf_route_sync_fill_entry(second, &job->ops[1].entry);
    }
    return job;
}

static int ospf_route_sync_enqueue(ospf_route_sync_job_t *job)
{
    if (!job || !g_ospf_route_sync)
    {
        g_free(job);
        return ERRCODE_FAIL;
    }

    g_mutex_lock(&g_ospf_route_sync->mutex);
    if (!g_ospf_route_sync->accepting)
    {
        g_mutex_unlock(&g_ospf_route_sync->mutex);
        g_free(job);
        return ERRCODE_FAIL;
    }
    g_queue_push_tail(g_ospf_route_sync->queue, job);
    g_cond_signal(&g_ospf_route_sync->cond);
    g_mutex_unlock(&g_ospf_route_sync->mutex);
    return ERRCODE_SUCCESS;
}

int ospf_route_sync_enqueue_add(const ospf_route_t *route)
{
    return ospf_route_sync_enqueue(ospf_route_sync_job_create(route, OSPF_ROUTE_SYNC_ADD, NULL, OSPF_ROUTE_SYNC_ADD));
}

int ospf_route_sync_enqueue_del(const ospf_route_t *route)
{
    return ospf_route_sync_enqueue(ospf_route_sync_job_create(route, OSPF_ROUTE_SYNC_DEL, NULL, OSPF_ROUTE_SYNC_DEL));
}

int ospf_route_sync_enqueue_replace(const ospf_route_t *current, const ospf_route_t *desired)
{
    return ospf_route_sync_enqueue(
        ospf_route_sync_job_create(current, OSPF_ROUTE_SYNC_DEL, desired, OSPF_ROUTE_SYNC_ADD));
}

static gboolean ospf_route_sync_is_stopping(uint32_t *final_wait_ms)
{
    gboolean stopping;
    g_mutex_lock(&g_ospf_route_sync->mutex);
    stopping = g_ospf_route_sync->stopping;
    if (final_wait_ms)
    {
        *final_wait_ms = 0u;
        gint64 remaining_usec = g_ospf_route_sync->shutdown_deadline_usec - g_get_monotonic_time();
        if (stopping && remaining_usec > 0)
        {
            uint64_t remaining_ms = ((uint64_t)remaining_usec + 999u) / 1000u;
            *final_wait_ms = (uint32_t)MIN(remaining_ms, (uint64_t)OSPF_ROUTE_SYNC_RPC_TIMEOUT_MS);
        }
    }
    g_mutex_unlock(&g_ospf_route_sync->mutex);
    return stopping;
}

static gboolean ospf_route_sync_retry_wait(uint32_t delay_ms)
{
    gboolean stopping;
    gint64 deadline = g_get_monotonic_time() + ((gint64)delay_ms * 1000);

    g_mutex_lock(&g_ospf_route_sync->mutex);
    while (!g_ospf_route_sync->stopping && g_get_monotonic_time() < deadline)
    {
        (void)g_cond_wait_until(&g_ospf_route_sync->cond, &g_ospf_route_sync->mutex, deadline);
    }
    stopping = g_ospf_route_sync->stopping;
    g_mutex_unlock(&g_ospf_route_sync->mutex);
    return stopping;
}

static void ospf_route_sync_process_op(const ospf_route_sync_op_t *op)
{
    uint32_t attempt = 0u;
    uint32_t delay_ms = OSPF_ROUTE_SYNC_RETRY_MIN_MS;

    for (;;)
    {
        uint32_t final_wait_ms = 0u;
        gboolean stopping = ospf_route_sync_is_stopping(&final_wait_ms);
        if (stopping)
        {
            if (op->type == OSPF_ROUTE_SYNC_DEL)
            {
                int rc = final_wait_ms > 0u ? route_rpc_del_wait(ospf_local_ipc_ctx(), &op->entry, final_wait_ms)
                                            : route_rpc_del(ospf_local_ipc_ctx(), &op->entry);
                if (rc != ERRCODE_SUCCESS)
                {
                    LOG_WARN("OSPF: final ROUTE withdrawal was not acknowledged");
                }
            }
            return;
        }

        int rc = (op->type == OSPF_ROUTE_SYNC_DEL)
                     ? route_rpc_del_wait(ospf_local_ipc_ctx(), &op->entry, OSPF_ROUTE_SYNC_RPC_TIMEOUT_MS)
                     : route_rpc_add_wait(ospf_local_ipc_ctx(), &op->entry, OSPF_ROUTE_SYNC_RPC_TIMEOUT_MS);
        if (rc == ERRCODE_SUCCESS)
        {
            return;
        }

        attempt++;
        if (attempt == 1u || (attempt % 30u) == 0u)
        {
            LOG_WARN("OSPF: ROUTE %s failed (attempt %u), retrying in %u ms",
                     op->type == OSPF_ROUTE_SYNC_DEL ? "withdraw" : "install", attempt, delay_ms);
        }
        if (ospf_route_sync_retry_wait(delay_ms))
        {
            continue;
        }
        delay_ms = MIN(delay_ms * 2u, OSPF_ROUTE_SYNC_RETRY_MAX_MS);
    }
}

static void ospf_route_sync_process_job(ospf_route_sync_job_t *job)
{
    for (uint8_t i = 0u; i < job->op_count; ++i)
    {
        ospf_route_sync_process_op(&job->ops[i]);
    }
}

static void *ospf_route_sync_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ospf-route");
    log_set_tag("ospf");

    for (;;)
    {
        g_mutex_lock(&g_ospf_route_sync->mutex);
        while (g_queue_is_empty(g_ospf_route_sync->queue) && !g_ospf_route_sync->stopping)
        {
            g_cond_wait(&g_ospf_route_sync->cond, &g_ospf_route_sync->mutex);
        }
        if (g_queue_is_empty(g_ospf_route_sync->queue) && g_ospf_route_sync->stopping)
        {
            g_mutex_unlock(&g_ospf_route_sync->mutex);
            break;
        }
        ospf_route_sync_job_t *job = g_queue_pop_head(g_ospf_route_sync->queue);
        g_mutex_unlock(&g_ospf_route_sync->mutex);

        ospf_route_sync_process_job(job);
        g_free(job);
    }
    return NULL;
}

int ospf_route_sync_prepare(void)
{
    if (g_ospf_route_sync)
    {
        return ERRCODE_SUCCESS;
    }

    ospf_route_sync_local_t *local = g_malloc0(sizeof(*local));
    if (!local)
    {
        return ERRCODE_FAIL;
    }
    g_mutex_init(&local->mutex);
    g_cond_init(&local->cond);
    local->queue = g_queue_new();
    if (!local->queue)
    {
        g_cond_clear(&local->cond);
        g_mutex_clear(&local->mutex);
        g_free(local);
        return ERRCODE_FAIL;
    }
    local->accepting = TRUE;
    g_ospf_route_sync = local;
    return ERRCODE_SUCCESS;
}

int ospf_route_sync_launch(void)
{
    if (!g_ospf_route_sync)
    {
        return ERRCODE_FAIL;
    }
    if (g_ospf_route_sync->thread_started)
    {
        return ERRCODE_SUCCESS;
    }
    if (pthread_create(&g_ospf_route_sync->thread, NULL, ospf_route_sync_thread, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    g_ospf_route_sync->thread_started = TRUE;
    return ERRCODE_SUCCESS;
}

void ospf_route_sync_shutdown(void)
{
    ospf_route_sync_local_t *local = g_ospf_route_sync;
    if (!local)
    {
        return;
    }

    g_mutex_lock(&local->mutex);
    local->accepting = FALSE;
    local->stopping = TRUE;
    local->shutdown_deadline_usec = g_get_monotonic_time() + ((gint64)OSPF_ROUTE_SYNC_SHUTDOWN_TIMEOUT_MS * 1000);
    g_cond_broadcast(&local->cond);
    g_mutex_unlock(&local->mutex);

    if (local->thread_started)
    {
        pthread_join(local->thread, NULL);
        local->thread_started = FALSE;
    }

    ospf_route_sync_job_t *job = NULL;
    while ((job = g_queue_pop_head(local->queue)) != NULL)
    {
        g_free(job);
    }
    g_queue_free(local->queue);
    g_cond_clear(&local->cond);
    g_mutex_clear(&local->mutex);
    g_free(local);
    g_ospf_route_sync = NULL;
}
