/**
 * @file   ospfv3_route_sync.c
 * @brief  Ordered asynchronous synchronization of OSPFV3 routes to ROUTE
 */
#include "ospfv3_route_sync.h"

#include <pthread.h>
#include <string.h>

#include "errcode.h"
#include "log.h"
#include "ospfv3_main.h"
#include "ospfv3_worker.h"
#include "route.h"

#define OSPFV3_ROUTE_SYNC_RPC_TIMEOUT_MS 500u
#define OSPFV3_ROUTE_SYNC_RETRY_MIN_MS 100u
#define OSPFV3_ROUTE_SYNC_RETRY_MAX_MS 1000u
#define OSPFV3_ROUTE_SYNC_SHUTDOWN_TIMEOUT_MS 3000u

typedef enum ospfv3_route_sync_op_type
{
    OSPFV3_ROUTE_SYNC_ADD = 1,
    OSPFV3_ROUTE_SYNC_DEL = 2,
} ospfv3_route_sync_op_type_t;

typedef struct ospfv3_route_sync_op
{
    ospfv3_route_sync_op_type_t type;
    route_msg_entry_t entry;
} ospfv3_route_sync_op_t;

typedef struct ospfv3_route_sync_job
{
    uint8_t op_count;
    ospfv3_route_sync_op_t ops[2];
} ospfv3_route_sync_job_t;

typedef struct ospfv3_route_sync_local
{
    GMutex mutex;
    GCond cond;
    GQueue *queue;
    pthread_t thread;
    gboolean accepting;
    gboolean stopping;
    gboolean thread_started;
    gint64 shutdown_deadline_usec;
} ospfv3_route_sync_local_t;

static ospfv3_route_sync_local_t *g_ospfv3_route_sync;

static void ospfv3_route_sync_fill_entry(const ospfv3_route_t *route, route_msg_entry_t *entry)
{
    memset(entry, 0, sizeof(*entry));
    entry->vrf_id = route->vrf_id;
    entry->afi = ROUTE_AFI_IPV6;
    entry->safi = ROUTE_SAFI_UNICAST;
    entry->prefix_len = route->prefix_len;
    entry->protocol = ROUTE_PROTOCOL_OSPFV3;
    entry->metric = (int32_t)route->metric;
    entry->preference = ROUTE_ADMIN_DIST_OSPFV3;
    entry->nh_type = ROUTE_NH_TYPE_IP;
    entry->out_ifindex = route->out_ifindex;
    entry->prefix_addr = route->prefix;
    entry->source_addr = route->source;
    entry->nexthop_addr = route->nexthop;
}

static ospfv3_route_sync_job_t *ospfv3_route_sync_job_create(const ospfv3_route_t *first,
                                                             ospfv3_route_sync_op_type_t first_type,
                                                             const ospfv3_route_t *second,
                                                             ospfv3_route_sync_op_type_t second_type)
{
    if (!first)
    {
        return NULL;
    }

    ospfv3_route_sync_job_t *job = g_malloc0(sizeof(*job));
    if (!job)
    {
        return NULL;
    }

    job->op_count = second ? 2u : 1u;
    job->ops[0].type = first_type;
    ospfv3_route_sync_fill_entry(first, &job->ops[0].entry);
    if (second)
    {
        job->ops[1].type = second_type;
        ospfv3_route_sync_fill_entry(second, &job->ops[1].entry);
    }
    return job;
}

static int ospfv3_route_sync_enqueue(ospfv3_route_sync_job_t *job)
{
    if (!job || !g_ospfv3_route_sync)
    {
        g_free(job);
        return ERRCODE_FAIL;
    }

    g_mutex_lock(&g_ospfv3_route_sync->mutex);
    if (!g_ospfv3_route_sync->accepting)
    {
        g_mutex_unlock(&g_ospfv3_route_sync->mutex);
        g_free(job);
        return ERRCODE_FAIL;
    }
    g_queue_push_tail(g_ospfv3_route_sync->queue, job);
    g_cond_signal(&g_ospfv3_route_sync->cond);
    g_mutex_unlock(&g_ospfv3_route_sync->mutex);
    return ERRCODE_SUCCESS;
}

int ospfv3_route_sync_enqueue_add(const ospfv3_route_t *route)
{
    return ospfv3_route_sync_enqueue(
        ospfv3_route_sync_job_create(route, OSPFV3_ROUTE_SYNC_ADD, NULL, OSPFV3_ROUTE_SYNC_ADD));
}

int ospfv3_route_sync_enqueue_del(const ospfv3_route_t *route)
{
    return ospfv3_route_sync_enqueue(
        ospfv3_route_sync_job_create(route, OSPFV3_ROUTE_SYNC_DEL, NULL, OSPFV3_ROUTE_SYNC_DEL));
}

int ospfv3_route_sync_enqueue_replace(const ospfv3_route_t *current, const ospfv3_route_t *desired)
{
    return ospfv3_route_sync_enqueue(
        ospfv3_route_sync_job_create(current, OSPFV3_ROUTE_SYNC_DEL, desired, OSPFV3_ROUTE_SYNC_ADD));
}

static gboolean ospfv3_route_sync_is_stopping(uint32_t *final_wait_ms)
{
    gboolean stopping;
    g_mutex_lock(&g_ospfv3_route_sync->mutex);
    stopping = g_ospfv3_route_sync->stopping;
    if (final_wait_ms)
    {
        *final_wait_ms = 0u;
        gint64 remaining_usec = g_ospfv3_route_sync->shutdown_deadline_usec - g_get_monotonic_time();
        if (stopping && remaining_usec > 0)
        {
            uint64_t remaining_ms = ((uint64_t)remaining_usec + 999u) / 1000u;
            *final_wait_ms = (uint32_t)MIN(remaining_ms, (uint64_t)OSPFV3_ROUTE_SYNC_RPC_TIMEOUT_MS);
        }
    }
    g_mutex_unlock(&g_ospfv3_route_sync->mutex);
    return stopping;
}

static gboolean ospfv3_route_sync_retry_wait(uint32_t delay_ms)
{
    gboolean stopping;
    gint64 deadline = g_get_monotonic_time() + ((gint64)delay_ms * 1000);

    g_mutex_lock(&g_ospfv3_route_sync->mutex);
    while (!g_ospfv3_route_sync->stopping && g_get_monotonic_time() < deadline)
    {
        (void)g_cond_wait_until(&g_ospfv3_route_sync->cond, &g_ospfv3_route_sync->mutex, deadline);
    }
    stopping = g_ospfv3_route_sync->stopping;
    g_mutex_unlock(&g_ospfv3_route_sync->mutex);
    return stopping;
}

static void ospfv3_route_sync_process_op(const ospfv3_route_sync_op_t *op)
{
    uint32_t attempt = 0u;
    uint32_t delay_ms = OSPFV3_ROUTE_SYNC_RETRY_MIN_MS;

    for (;;)
    {
        uint32_t final_wait_ms = 0u;
        gboolean stopping = ospfv3_route_sync_is_stopping(&final_wait_ms);
        if (stopping)
        {
            if (op->type == OSPFV3_ROUTE_SYNC_DEL)
            {
                int rc = final_wait_ms > 0u ? route_rpc_del_wait(ospfv3_local_ipc_ctx(), &op->entry, final_wait_ms)
                                            : route_rpc_del(ospfv3_local_ipc_ctx(), &op->entry);
                if (rc != ERRCODE_SUCCESS)
                {
                    LOG_WARN("OSPFV3: final ROUTE withdrawal was not acknowledged");
                }
            }
            return;
        }

        int rc = (op->type == OSPFV3_ROUTE_SYNC_DEL)
                     ? route_rpc_del_wait(ospfv3_local_ipc_ctx(), &op->entry, OSPFV3_ROUTE_SYNC_RPC_TIMEOUT_MS)
                     : route_rpc_add_wait(ospfv3_local_ipc_ctx(), &op->entry, OSPFV3_ROUTE_SYNC_RPC_TIMEOUT_MS);
        if (rc == ERRCODE_SUCCESS)
        {
            return;
        }

        attempt++;
        if (attempt == 1u || (attempt % 30u) == 0u)
        {
            LOG_WARN("OSPFV3: ROUTE %s failed (attempt %u), retrying in %u ms",
                     op->type == OSPFV3_ROUTE_SYNC_DEL ? "withdraw" : "install", attempt, delay_ms);
        }
        if (ospfv3_route_sync_retry_wait(delay_ms))
        {
            continue;
        }
        delay_ms = MIN(delay_ms * 2u, OSPFV3_ROUTE_SYNC_RETRY_MAX_MS);
    }
}

static void ospfv3_route_sync_process_job(ospfv3_route_sync_job_t *job)
{
    for (uint8_t i = 0u; i < job->op_count; ++i)
    {
        ospfv3_route_sync_process_op(&job->ops[i]);
    }
}

static void *ospfv3_route_sync_thread(void *arg)
{
    (void)arg;
    pthread_setname_np(pthread_self(), "ospfv3-route");
    log_set_tag("ospfv3");

    for (;;)
    {
        g_mutex_lock(&g_ospfv3_route_sync->mutex);
        while (g_queue_is_empty(g_ospfv3_route_sync->queue) && !g_ospfv3_route_sync->stopping)
        {
            g_cond_wait(&g_ospfv3_route_sync->cond, &g_ospfv3_route_sync->mutex);
        }
        if (g_queue_is_empty(g_ospfv3_route_sync->queue) && g_ospfv3_route_sync->stopping)
        {
            g_mutex_unlock(&g_ospfv3_route_sync->mutex);
            break;
        }
        ospfv3_route_sync_job_t *job = g_queue_pop_head(g_ospfv3_route_sync->queue);
        g_mutex_unlock(&g_ospfv3_route_sync->mutex);

        ospfv3_route_sync_process_job(job);
        g_free(job);
    }
    return NULL;
}

int ospfv3_route_sync_prepare(void)
{
    if (g_ospfv3_route_sync)
    {
        return ERRCODE_SUCCESS;
    }

    ospfv3_route_sync_local_t *local = g_malloc0(sizeof(*local));
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
    g_ospfv3_route_sync = local;
    return ERRCODE_SUCCESS;
}

int ospfv3_route_sync_launch(void)
{
    if (!g_ospfv3_route_sync)
    {
        return ERRCODE_FAIL;
    }
    if (g_ospfv3_route_sync->thread_started)
    {
        return ERRCODE_SUCCESS;
    }
    if (pthread_create(&g_ospfv3_route_sync->thread, NULL, ospfv3_route_sync_thread, NULL) != 0)
    {
        return ERRCODE_FAIL;
    }
    g_ospfv3_route_sync->thread_started = TRUE;
    return ERRCODE_SUCCESS;
}

void ospfv3_route_sync_shutdown(void)
{
    ospfv3_route_sync_local_t *local = g_ospfv3_route_sync;
    if (!local)
    {
        return;
    }

    g_mutex_lock(&local->mutex);
    local->accepting = FALSE;
    local->stopping = TRUE;
    local->shutdown_deadline_usec = g_get_monotonic_time() + ((gint64)OSPFV3_ROUTE_SYNC_SHUTDOWN_TIMEOUT_MS * 1000);
    g_cond_broadcast(&local->cond);
    g_mutex_unlock(&local->mutex);

    if (local->thread_started)
    {
        pthread_join(local->thread, NULL);
        local->thread_started = FALSE;
    }

    ospfv3_route_sync_job_t *job = NULL;
    while ((job = g_queue_pop_head(local->queue)) != NULL)
    {
        g_free(job);
    }
    g_queue_free(local->queue);
    g_cond_clear(&local->cond);
    g_mutex_clear(&local->mutex);
    g_free(local);
    g_ospfv3_route_sync = NULL;
}
