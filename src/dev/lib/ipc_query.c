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
    pthread_mutex_destroy(&pq->mutex);
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

uint32_t dev_ipc_query_mgr_register(dev_ipc_query_mgr_t *mgr)
{
    pthread_mutex_lock(&mgr->lock);

    uint32_t id = mgr->next_id++;
    if (mgr->next_id == 0)
    {
        mgr->next_id = 1; /* 避免 0 */
    }

    dev_ipc_pending_query_t *pq = g_malloc0(sizeof(dev_ipc_pending_query_t));
    pq->request_id = id;
    pq->response = NULL;
    pq->completed = 0;
    pthread_mutex_init(&pq->mutex, NULL);
    pthread_cond_init(&pq->cond, NULL);

    g_hash_table_insert(mgr->pending, GUINT_TO_POINTER(id), pq);

    pthread_mutex_unlock(&mgr->lock);
    return id;
}

dev_ipc_message_t *dev_ipc_query_mgr_wait(dev_ipc_query_mgr_t *mgr, uint32_t request_id, uint32_t timeout_ms)
{
    /* 查找挂起查询 */
    pthread_mutex_lock(&mgr->lock);
    dev_ipc_pending_query_t *pq = g_hash_table_lookup(mgr->pending, GUINT_TO_POINTER(request_id));
    pthread_mutex_unlock(&mgr->lock);

    if (!pq)
    {
        return NULL;
    }

    /* 等待响应 */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000)
    {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&pq->mutex);
    while (!pq->completed)
    {
        int ret = pthread_cond_timedwait(&pq->cond, &pq->mutex, &ts);
        if (ret != 0)
        {
            break; /* 超时或错误 */
        }
    }
    dev_ipc_message_t *response = pq->response;
    pq->response = NULL; /* 转移所有权 */
    pthread_mutex_unlock(&pq->mutex);

    /* 清理挂起查询 */
    pthread_mutex_lock(&mgr->lock);
    g_hash_table_remove(mgr->pending, GUINT_TO_POINTER(request_id));
    pthread_mutex_unlock(&mgr->lock);

    return response;
}

int dev_ipc_query_mgr_complete(dev_ipc_query_mgr_t *mgr, uint32_t request_id, dev_ipc_message_t *response)
{
    pthread_mutex_lock(&mgr->lock);
    dev_ipc_pending_query_t *pq = g_hash_table_lookup(mgr->pending, GUINT_TO_POINTER(request_id));
    pthread_mutex_unlock(&mgr->lock);

    if (!pq)
    {
        return ERRCODE_FAIL;
    }

    pthread_mutex_lock(&pq->mutex);
    pq->response = response;
    pq->completed = 1;
    pthread_cond_signal(&pq->cond);
    pthread_mutex_unlock(&pq->mutex);

    return ERRCODE_SUCCESS;
}
