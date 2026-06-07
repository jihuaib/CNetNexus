/**
 * @file   bgp_bulk.c
 * @brief  BGP bulk task scheduler helpers
 */
#include "bgp_bulk.h"

#include <string.h>

#include "bgp_worker.h"
#include "log.h"

#define BGP_BULK_HEAD_BUDGET BGP_WORK_BATCH_SIZE

typedef enum bgp_bulk_task_kind
{
    BGP_BULK_TASK_INST_RIB_WALK = 1,
} bgp_bulk_task_kind_t;

struct bgp_bulk_task
{
    bgp_bulk_task_kind_t kind;
    uint32_t vrf_id;
    bgp_afi_t afi;
    bgp_safi_t safi;

    GArray *rd_keys; /* bgp_rd_t 值拷贝 */
    guint rd_index;
    gboolean rd_snapshot_done;

    bgp_nlri_entry_t last_nlri;
    gboolean has_last_nlri;

    bgp_bulk_inst_head_cb head_cb;
    bgp_bulk_done_cb done_cb;
    gpointer user_data;
    GDestroyNotify destroy;
};

static gboolean bulk_snapshot_rd_keys(bgp_bulk_task_t *task, bgp_instance_t *inst)
{
    if (!task || !inst || !inst->rd_entries)
    {
        return FALSE;
    }
    if (task->rd_snapshot_done)
    {
        return TRUE;
    }

    task->rd_keys = g_array_new(FALSE, FALSE, sizeof(bgp_rd_t));
    if (!task->rd_keys)
    {
        return FALSE;
    }

    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, inst->rd_entries);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        (void)value;
        if (key)
        {
            bgp_rd_t rd;
            memcpy(&rd, key, sizeof(rd));
            g_array_append_val(task->rd_keys, rd);
        }
    }
    task->rd_snapshot_done = TRUE;
    return TRUE;
}

bgp_bulk_task_t *bgp_bulk_inst_rib_walk_create(uint32_t vrf_id, bgp_afi_t afi, bgp_safi_t safi,
                                               bgp_bulk_inst_head_cb head_cb, bgp_bulk_done_cb done_cb,
                                               gpointer user_data, GDestroyNotify destroy)
{
    if (!head_cb)
    {
        return NULL;
    }

    bgp_bulk_task_t *task = g_malloc0(sizeof(*task));
    if (!task)
    {
        return NULL;
    }

    task->kind = BGP_BULK_TASK_INST_RIB_WALK;
    task->vrf_id = vrf_id;
    task->afi = afi;
    task->safi = safi;
    task->head_cb = head_cb;
    task->done_cb = done_cb;
    task->user_data = user_data;
    task->destroy = destroy;
    return task;
}

void bgp_bulk_task_destroy(bgp_bulk_task_t *task)
{
    if (!task)
    {
        return;
    }
    if (task->rd_keys)
    {
        g_array_free(task->rd_keys, TRUE);
    }
    if (task->destroy)
    {
        task->destroy(task->user_data);
    }
    g_free(task);
}

typedef struct bulk_head_cb_ctx
{
    bgp_bulk_task_t *task;
    bgp_instance_t *inst;
    bgp_rd_entry_t *entry;
} bulk_head_cb_ctx_t;

static gboolean bulk_walk_head_cb(bgp_rthead_t *head, gpointer user_data)
{
    bulk_head_cb_ctx_t *ctx = (bulk_head_cb_ctx_t *)user_data;
    if (!ctx || !ctx->task || !ctx->task->head_cb)
    {
        return FALSE;
    }
    ctx->task->head_cb(ctx->inst, ctx->entry, head, ctx->task->user_data);
    return TRUE;
}

static gboolean bulk_task_inst_rib_walk_step(bgp_bulk_task_t *task)
{
    bgp_instance_t *inst = bgp_worker_lookup_instance(task->vrf_id, task->afi, task->safi);
    if (!inst || !inst->rd_entries)
    {
        return TRUE;
    }
    if (!bulk_snapshot_rd_keys(task, inst))
    {
        return TRUE;
    }

    uint32_t remaining = BGP_BULK_HEAD_BUDGET;
    while (remaining > 0 && task->rd_keys && task->rd_index < task->rd_keys->len)
    {
        bgp_rd_t *rd = &g_array_index(task->rd_keys, bgp_rd_t, task->rd_index);
        bgp_rd_entry_t *entry = (bgp_rd_entry_t *)g_hash_table_lookup(inst->rd_entries, rd);
        if (!entry || !entry->rib)
        {
            task->rd_index++;
            task->has_last_nlri = FALSE;
            continue;
        }

        gboolean walked = FALSE;
        uint32_t processed = 0;
        bgp_nlri_entry_t new_last;
        bulk_head_cb_ctx_t cb_ctx = {.task = task, .inst = inst, .entry = entry};
        gboolean done = bgp_rib_walk_heads_from(entry->rib, &task->last_nlri, task->has_last_nlri, remaining,
                                                bulk_walk_head_cb, &cb_ctx, &new_last, &walked, &processed);
        if (walked)
        {
            memcpy(&task->last_nlri, &new_last, sizeof(task->last_nlri));
            task->has_last_nlri = TRUE;
            remaining = (processed >= remaining) ? 0 : (remaining - processed);
        }
        if (!done)
        {
            return FALSE;
        }

        task->rd_index++;
        task->has_last_nlri = FALSE;
    }

    return task->rd_keys == NULL || task->rd_index >= task->rd_keys->len;
}

gboolean bgp_bulk_task_handle(bgp_bulk_task_t *task)
{
    if (!task)
    {
        return TRUE;
    }

    gboolean done = TRUE;
    switch (task->kind)
    {
        case BGP_BULK_TASK_INST_RIB_WALK:
            done = bulk_task_inst_rib_walk_step(task);
            break;
        default:
            LOG_WARN("BGP bulk: unknown task kind=%d", (int)task->kind);
            done = TRUE;
            break;
    }

    if (done)
    {
        if (task->done_cb)
        {
            task->done_cb(task->user_data);
        }
        return TRUE;
    }

    if (bgp_worker_post_bulk_task(task) != 0)
    {
        LOG_WARN("BGP bulk: failed to reschedule task kind=%d", (int)task->kind);
        return TRUE;
    }
    return FALSE;
}
