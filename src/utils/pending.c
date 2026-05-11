/**
 * @file   pending.c
 * @brief  通用挂起表实现
 * @author jhb
 * @date   2026/05/11
 */

#include "pending.h"

#include <glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log.h"

/** 依赖键：类型 + 64 位键值组合 */
typedef struct
{
    uint32_t dep_type;
    uint64_t dep_key;
} pending_dep_key_t;

/** 单个挂起项 */
typedef struct
{
    void *item;                   /**< 数据指针（副本或原引用） */
    size_t item_size;             /**< >0 表示是堆上的副本，==0 表示原引用 */
    pending_item_free_fn free_fn; /**< 引用模式下的释放回调，可为 NULL */
    pending_retry_fn retry_fn;    /**< 重试回调 */
    void *ctx;                    /**< 透传给 retry_fn 的上下文 */
    struct timespec parked_at;    /**< 挂起时刻（CLOCK_MONOTONIC），用于显示时长 */
    uint32_t dep_type;            /**< 冗余保存：dump 时使用 */
    uint64_t dep_key;             /**< 冗余保存：dump 时使用 */
} pending_item_t;

/** 挂起表 */
struct pending
{
    char *name;
    GHashTable *table; /**< key=pending_dep_key_t*, value=GList* of pending_item_t* */
    size_t count;
};

/* ---------- 内部辅助 ---------- */

static guint dep_key_hash(gconstpointer p)
{
    const pending_dep_key_t *k = p;
    /* 简单混合 */
    uint64_t h = (uint64_t)k->dep_type * 0x9E3779B97F4A7C15ULL;
    h ^= k->dep_key + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return (guint)(h ^ (h >> 32));
}

static gboolean dep_key_equal(gconstpointer a, gconstpointer b)
{
    const pending_dep_key_t *x = a;
    const pending_dep_key_t *y = b;
    return x->dep_type == y->dep_type && x->dep_key == y->dep_key;
}

static void item_free(pending_item_t *it)
{
    if (!it)
    {
        return;
    }
    if (it->item_size > 0)
    {
        g_free(it->item);
    }
    else if (it->free_fn)
    {
        it->free_fn(it->item);
    }
    g_free(it);
}

static void item_list_free(gpointer data)
{
    GList *list = data;
    for (GList *l = list; l; l = l->next)
    {
        item_free(l->data);
    }
    g_list_free(list);
}

static double ts_elapsed_sec(const struct timespec *parked)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    double sec = (double)(now.tv_sec - parked->tv_sec);
    sec += (double)(now.tv_nsec - parked->tv_nsec) / 1e9;
    return sec;
}

/* ---------- 公共 API ---------- */

pending_t *pending_new(const char *name)
{
    pending_t *p = g_new0(pending_t, 1);
    p->name = g_strdup(name ? name : "pending");
    p->table = g_hash_table_new_full(dep_key_hash, dep_key_equal, g_free, item_list_free);
    p->count = 0;
    return p;
}

void pending_destroy(pending_t *p)
{
    if (!p)
    {
        return;
    }
    g_hash_table_destroy(p->table);
    g_free(p->name);
    g_free(p);
}

void pending_park(pending_t *p, uint32_t dep_type, uint64_t dep_key, const void *item, size_t item_size,
                  pending_item_free_fn free_fn, pending_retry_fn fn, void *ctx)
{
    if (!p || !fn)
    {
        LOG_ERROR("pending_park: invalid args (p=%p fn=%p)", (void *)p, (void *)fn);
        return;
    }

    pending_item_t *it = g_new0(pending_item_t, 1);
    it->item_size = item_size;
    it->free_fn = free_fn;
    it->retry_fn = fn;
    it->ctx = ctx;
    it->dep_type = dep_type;
    it->dep_key = dep_key;
    clock_gettime(CLOCK_MONOTONIC, &it->parked_at);

    if (item_size > 0)
    {
        it->item = g_malloc(item_size);
        memcpy(it->item, item, item_size);
    }
    else
    {
        it->item = (void *)item;
    }

    pending_dep_key_t lookup = {.dep_type = dep_type, .dep_key = dep_key};
    GList *list = g_hash_table_lookup(p->table, &lookup);
    if (list)
    {
        /* 已有非空列表：g_list_append 在已有 head 上原地链接尾节点，
         * 返回的仍是原 head，因此哈希表内的指针无需更新。 */
        (void)g_list_append(list, it);
    }
    else
    {
        pending_dep_key_t *k = g_new0(pending_dep_key_t, 1);
        *k = lookup;
        g_hash_table_insert(p->table, k, g_list_append(NULL, it));
    }
    p->count++;

    LOG_INFO("[%s] park: type=%u key=0x%016lx (total=%zu)", p->name, dep_type, (unsigned long)dep_key, p->count);
}

int pending_resolve(pending_t *p, uint32_t dep_type, uint64_t dep_key)
{
    if (!p)
    {
        return 0;
    }

    pending_dep_key_t lookup = {.dep_type = dep_type, .dep_key = dep_key};
    gpointer stolen_key = NULL;
    gpointer stolen_val = NULL;
    if (!g_hash_table_steal_extended(p->table, &lookup, &stolen_key, &stolen_val))
    {
        return 0;
    }
    /* 把列表从表中摘出，使重试期间 retry_fn 调用 pending_park 到相同 key
     * 不会污染当前迭代——会作为新条目插入哈希表。 */
    g_free(stolen_key);
    GList *list = stolen_val;

    int done = 0;
    size_t before = g_list_length(list);

    LOG_INFO("[%s] resolve: type=%u key=0x%016lx items=%zu", p->name, dep_type, (unsigned long)dep_key, before);

    for (GList *l = list; l; l = l->next)
    {
        pending_item_t *it = l->data;
        int rc = it->retry_fn(it->item, it->ctx);
        if (rc == PENDING_DONE)
        {
            done++;
            item_free(it);
        }
        else if (rc == PENDING_AGAIN)
        {
            /* retry_fn 应已自行 pending_park 到新 key；释放本副本 */
            item_free(it);
        }
        else
        {
            LOG_ERROR("[%s] retry failed: type=%u key=0x%016lx rc=%d", p->name, dep_type, (unsigned long)dep_key, rc);
            item_free(it);
        }
    }
    g_list_free(list);

    p->count -= before;
    return done;
}

int pending_invalidate(pending_t *p, uint32_t dep_type, uint64_t dep_key)
{
    if (!p)
    {
        return 0;
    }
    pending_dep_key_t lookup = {.dep_type = dep_type, .dep_key = dep_key};
    GList *list = g_hash_table_lookup(p->table, &lookup);
    if (!list)
    {
        return 0;
    }
    int n = (int)g_list_length(list);
    /* remove 会触发 key_destroy_func 和 value_destroy_func（item_list_free） */
    g_hash_table_remove(p->table, &lookup);
    p->count -= (size_t)n;
    LOG_INFO("[%s] invalidate: type=%u key=0x%016lx dropped=%d", p->name, dep_type, (unsigned long)dep_key, n);
    return n;
}

size_t pending_count(const pending_t *p)
{
    return p ? p->count : 0;
}

void pending_dump(const pending_t *p, FILE *out, pending_item_dump_fn dump_item)
{
    if (!p)
    {
        return;
    }
    FILE *fp = out ? out : stderr;

    fprintf(fp, "Pending table [%s]: %zu item(s)\n", p->name, p->count);

    GHashTableIter iter;
    gpointer key;
    gpointer value;
    g_hash_table_iter_init(&iter, p->table);
    while (g_hash_table_iter_next(&iter, &key, &value))
    {
        const pending_dep_key_t *k = key;
        GList *list = value;
        fprintf(fp, "  dep_type=%u dep_key=0x%016lx waiting=%u\n", k->dep_type, (unsigned long)k->dep_key,
                g_list_length(list));
        for (GList *l = list; l; l = l->next)
        {
            const pending_item_t *it = l->data;
            fprintf(fp, "    - parked %.1fs ago", ts_elapsed_sec(&it->parked_at));
            if (dump_item)
            {
                fprintf(fp, " ");
                dump_item(fp, it->dep_type, it->dep_key, it->item);
            }
            fprintf(fp, "\n");
        }
    }
}
