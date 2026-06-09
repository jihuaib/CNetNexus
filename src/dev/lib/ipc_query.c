/**
 * @file   dev_ipc_query.c
 * @brief  IPC 同步查询支持实现
 * @author jhb
 * @date   2026/02/02
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "dev.h"
#include "errcode.h"

static void free_pending_query(dev_ipc_pending_query_t *pq)
{
    if (!pq)
    {
        return;
    }
    pthread_cond_destroy(&pq->cond);
    if (pq->response)
    {
        dev_ipc_message_free(pq->response);
    }
    g_free(pq);
}

dev_ipc_query_mgr_t *dev_ipc_query_mgr_create(void)
{
    dev_ipc_query_mgr_t *mgr = g_malloc0(sizeof(dev_ipc_query_mgr_t));
    mgr->pending = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)free_pending_query);
    pthread_mutex_init(&mgr->lock, NULL);
    mgr->next_id = 1;
    return mgr;
}

void dev_ipc_query_mgr_destroy(dev_ipc_query_mgr_t *mgr)
{
    if (!mgr)
    {
        return;
    }
    pthread_mutex_lock(&mgr->lock);
    g_hash_table_destroy(mgr->pending);
    pthread_mutex_unlock(&mgr->lock);
    pthread_mutex_destroy(&mgr->lock);
    g_free(mgr);
}

uint32_t dev_ipc_query_mgr_register(dev_ipc_query_mgr_t *mgr, uint32_t target_module_id)
{
    if (!mgr)
    {
        return 0;
    }

    pthread_mutex_lock(&mgr->lock);

    uint32_t id = mgr->next_id++;
    if (mgr->next_id == 0 || (mgr->next_id & DEV_IPC_REQUEST_ID_RESPONSE_FLAG) != 0)
    {
        mgr->next_id = 1; /* 避免 0，并保留高位作为响应标记 */
    }

    dev_ipc_pending_query_t *pq = g_malloc0(sizeof(dev_ipc_pending_query_t));
    pq->request_id = id;
    pq->target_module_id = target_module_id;
    pq->response = NULL;
    pq->completed = 0;
    pthread_cond_init(&pq->cond, NULL);

    g_hash_table_insert(mgr->pending, GUINT_TO_POINTER(id), pq);

    pthread_mutex_unlock(&mgr->lock);
    return id;
}

dev_ipc_message_t *dev_ipc_query_mgr_wait(dev_ipc_query_mgr_t *mgr, uint32_t request_id, uint32_t timeout_ms)
{
    if (!mgr || request_id == 0)
    {
        return NULL;
    }

    pthread_mutex_lock(&mgr->lock);
    dev_ipc_pending_query_t *pq = g_hash_table_lookup(mgr->pending, GUINT_TO_POINTER(request_id));
    if (!pq)
    {
        pthread_mutex_unlock(&mgr->lock);
        return NULL;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    while (!pq->completed)
    {
        int ret = pthread_cond_timedwait(&pq->cond, &mgr->lock, &ts);
        if (ret != 0)
        {
            break; /* 超时或错误 */
        }
    }

    dev_ipc_message_t *response = pq->response;
    pq->response = NULL; /* 转移所有权 */
    g_hash_table_remove(mgr->pending, GUINT_TO_POINTER(request_id));
    pthread_mutex_unlock(&mgr->lock);

    return response;
}

int dev_ipc_query_mgr_complete(dev_ipc_query_mgr_t *mgr, uint32_t request_id, dev_ipc_message_t *response)
{
    if (!mgr || request_id == 0)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&mgr->lock);
    dev_ipc_pending_query_t *pq = g_hash_table_lookup(mgr->pending, GUINT_TO_POINTER(request_id));
    if (!pq)
    {
        pthread_mutex_unlock(&mgr->lock);
        return ERRCODE_FAIL;
    }

    if (pq->completed)
    {
        pthread_mutex_unlock(&mgr->lock);
        return ERRCODE_FAIL;
    }

    pq->response = response;
    pq->completed = 1;
    pthread_cond_signal(&pq->cond);
    pthread_mutex_unlock(&mgr->lock);

    return ERRCODE_SUCCESS;
}

void dev_ipc_query_mgr_cancel(dev_ipc_query_mgr_t *mgr, uint32_t request_id)
{
    if (!mgr || request_id == 0)
    {
        return;
    }

    pthread_mutex_lock(&mgr->lock);
    g_hash_table_remove(mgr->pending, GUINT_TO_POINTER(request_id));
    pthread_mutex_unlock(&mgr->lock);
}

void dev_ipc_query_mgr_cancel_by_target(dev_ipc_query_mgr_t *mgr, uint32_t target_module_id)
{
    if (!mgr || target_module_id == 0)
    {
        return;
    }

    pthread_mutex_lock(&mgr->lock);

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, mgr->pending);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        dev_ipc_pending_query_t *pq = (dev_ipc_pending_query_t *)value;
        if (!pq || pq->completed || pq->target_module_id != target_module_id)
        {
            continue;
        }
        /* 不释放 pq——等 wait() 醒来按正常路径移除 */
        pq->response = NULL;
        pq->completed = 1;
        pthread_cond_signal(&pq->cond);
    }

    pthread_mutex_unlock(&mgr->lock);
}
