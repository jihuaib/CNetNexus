/**
 * @file   cli_cfg_anchor.c
 * @brief  配置锚点框架: 发射器 + 聚合器实现
 * @author jhb
 * @date   2026/04/17
 */

#include "cli_cfg_anchor.h"

#include <stdint.h>
#include <string.h>

#include "log.h"

/* 默认段尾, 属主未声明 footer 时使用 */
#define CLI_CFG_ANCHOR_DEFAULT_FOOTER "!\r\n"

// ============================================================================
// 发射器 (emitter)
// ============================================================================

static gboolean key_is_valid(const char *key)
{
    if (!key || !key[0])
    {
        return FALSE;
    }

    size_t len = strlen(key);
    if (len >= CLI_CFG_ANCHOR_KEY_MAX)
    {
        return FALSE;
    }

    /* key 不得含控制字符或空白 */
    for (size_t i = 0; i < len; i++)
    {
        unsigned char c = (unsigned char)key[i];
        if (c < 0x20 || c == 0x7f || c == ' ' || c == '\t')
        {
            return FALSE;
        }
    }
    return TRUE;
}

static void emit_wrapped(GString *out, char kind, const char *key, const char *payload)
{
    if (!out)
    {
        return;
    }
    if (!key_is_valid(key))
    {
        LOG_WARN("cli_cfg_anchor: invalid key ignored");
        return;
    }

    g_string_append_c(out, CLI_CFG_ANCHOR_DELIM);
    g_string_append_c(out, kind);
    g_string_append_c(out, ' ');
    g_string_append(out, key);
    g_string_append_c(out, CLI_CFG_ANCHOR_DELIM);
    g_string_append_c(out, '\n');

    if (payload && payload[0])
    {
        g_string_append(out, payload);
    }

    g_string_append_c(out, CLI_CFG_ANCHOR_DELIM);
    g_string_append_c(out, CLI_CFG_ANCHOR_KIND_END);
    g_string_append_c(out, CLI_CFG_ANCHOR_DELIM);
    g_string_append_c(out, '\n');
}

void cli_cfg_anchor_emit_global(GString *out, const char *text)
{
    if (!out || !text)
    {
        return;
    }
    g_string_append(out, text);
}

void cli_cfg_anchor_emit_header(GString *out, const char *key, const char *header)
{
    emit_wrapped(out, CLI_CFG_ANCHOR_KIND_HEADER, key, header);
}

void cli_cfg_anchor_emit_body(GString *out, const char *key, const char *body)
{
    emit_wrapped(out, CLI_CFG_ANCHOR_KIND_BODY, key, body);
}

void cli_cfg_anchor_emit_footer(GString *out, const char *key, const char *footer)
{
    emit_wrapped(out, CLI_CFG_ANCHOR_KIND_FOOTER, key, footer);
}

// ============================================================================
// 聚合器 (aggregator)
// ============================================================================

typedef struct
{
    GString *header;
    GString *body;
    GString *footer;
    uint32_t order_seq;
    gboolean header_set;
    gboolean footer_set;
} anchor_bucket_t;

struct cli_cfg_anchor_aggregator
{
    GString *global;
    GHashTable *anchors; /* key: char*(dup)  value: anchor_bucket_t* */
    GList *order_keys;   /* 首次出现顺序, 值指向 hash 中的 key(不单独 own) */
    uint32_t seq_counter;
};

static anchor_bucket_t *bucket_new(void)
{
    anchor_bucket_t *b = g_new0(anchor_bucket_t, 1);
    b->header = g_string_new("");
    b->body = g_string_new("");
    b->footer = g_string_new("");
    return b;
}

static void bucket_free(gpointer data)
{
    anchor_bucket_t *b = (anchor_bucket_t *)data;
    if (!b)
    {
        return;
    }
    if (b->header)
    {
        g_string_free(b->header, TRUE);
    }
    if (b->body)
    {
        g_string_free(b->body, TRUE);
    }
    if (b->footer)
    {
        g_string_free(b->footer, TRUE);
    }
    g_free(b);
}

cli_cfg_anchor_aggregator_t *cli_cfg_anchor_agg_new(void)
{
    cli_cfg_anchor_aggregator_t *agg = g_new0(cli_cfg_anchor_aggregator_t, 1);
    agg->global = g_string_new("");
    agg->anchors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, bucket_free);
    agg->order_keys = NULL;
    agg->seq_counter = 0;
    return agg;
}

void cli_cfg_anchor_agg_free(cli_cfg_anchor_aggregator_t *agg)
{
    if (!agg)
    {
        return;
    }
    if (agg->global)
    {
        g_string_free(agg->global, TRUE);
    }
    if (agg->anchors)
    {
        g_hash_table_destroy(agg->anchors);
    }
    if (agg->order_keys)
    {
        g_list_free(agg->order_keys);
    }
    g_free(agg);
}

static anchor_bucket_t *ensure_bucket(cli_cfg_anchor_aggregator_t *agg, const char *key)
{
    anchor_bucket_t *bucket = g_hash_table_lookup(agg->anchors, key);
    if (bucket)
    {
        return bucket;
    }

    bucket = bucket_new();
    bucket->order_seq = ++agg->seq_counter;
    char *key_dup = g_strdup(key);
    g_hash_table_insert(agg->anchors, key_dup, bucket);

    /* GLib 保留 hash key 的指针指向 key_dup, 直到 bucket 被销毁, 因此顺序
     * 列表直接引用同一指针, 无需再复制 */
    agg->order_keys = g_list_append(agg->order_keys, key_dup);
    return bucket;
}

/**
 * @brief 在 [pos, len) 区间找到 CLI_CFG_ANCHOR_DELIM 的位置; 未命中返回 len
 */
static gsize find_delim(const char *buf, gsize len, gsize pos)
{
    for (gsize i = pos; i < len; i++)
    {
        if (buf[i] == CLI_CFG_ANCHOR_DELIM)
        {
            return i;
        }
    }
    return len;
}

/**
 * @brief 尝试从 buf[pos] 解析 "\x01<kind> <key>\x01\n";
 *        成功: 返回标志头后第一个字节的偏移, 并填 kind/key 输出;
 *        失败: 返回 0(调用方应按"非标志普通字节"处理)。
 */
static gsize try_parse_header(const char *buf, gsize len, gsize pos, char *kind_out, char *key_out, gsize key_cap)
{
    /* 形如: \x01 <kind> ' ' <key...> \x01 \n  */
    if (pos + 4 > len || buf[pos] != CLI_CFG_ANCHOR_DELIM)
    {
        return 0;
    }

    char kind = buf[pos + 1];
    if (kind != CLI_CFG_ANCHOR_KIND_HEADER && kind != CLI_CFG_ANCHOR_KIND_BODY && kind != CLI_CFG_ANCHOR_KIND_FOOTER)
    {
        return 0;
    }
    if (buf[pos + 2] != ' ')
    {
        return 0;
    }

    gsize key_start = pos + 3;
    gsize delim_end = find_delim(buf, len, key_start);
    if (delim_end == len)
    {
        return 0;
    }
    gsize key_len = delim_end - key_start;
    if (key_len == 0 || key_len >= key_cap)
    {
        return 0;
    }
    if (delim_end + 1 >= len || buf[delim_end + 1] != '\n')
    {
        return 0;
    }

    memcpy(key_out, buf + key_start, key_len);
    key_out[key_len] = '\0';
    *kind_out = kind;
    return delim_end + 2; /* 跳过 \x01 和 \n */
}

/**
 * @brief 从 payload_start 扫描直到 "\x01E\x01\n"; 返回 end 标记 pos 与 payload 字节数
 */
static gboolean find_end_marker(const char *buf, gsize len, gsize payload_start, gsize *end_marker_pos_out,
                                gsize *payload_bytes_out)
{
    for (gsize i = payload_start; i + 3 < len; i++)
    {
        if (buf[i] == CLI_CFG_ANCHOR_DELIM && buf[i + 1] == CLI_CFG_ANCHOR_KIND_END &&
            buf[i + 2] == CLI_CFG_ANCHOR_DELIM && buf[i + 3] == '\n')
        {
            *end_marker_pos_out = i;
            *payload_bytes_out = i - payload_start;
            return TRUE;
        }
    }
    return FALSE;
}

static void apply_payload_to_bucket(cli_cfg_anchor_aggregator_t *agg, char kind, const char *key, const char *payload,
                                    gsize payload_len)
{
    anchor_bucket_t *bucket = ensure_bucket(agg, key);

    switch (kind)
    {
        case CLI_CFG_ANCHOR_KIND_HEADER:
            if (bucket->header_set)
            {
                LOG_WARN("cli_cfg_anchor: duplicate header for key='%s' ignored", key);
                return;
            }
            g_string_append_len(bucket->header, payload, payload_len);
            bucket->header_set = TRUE;
            break;
        case CLI_CFG_ANCHOR_KIND_BODY:
            g_string_append_len(bucket->body, payload, payload_len);
            break;
        case CLI_CFG_ANCHOR_KIND_FOOTER:
            if (bucket->footer_set)
            {
                LOG_WARN("cli_cfg_anchor: duplicate footer for key='%s' ignored", key);
                return;
            }
            g_string_append_len(bucket->footer, payload, payload_len);
            bucket->footer_set = TRUE;
            break;
        default:
            break;
    }
}

void cli_cfg_anchor_agg_feed(cli_cfg_anchor_aggregator_t *agg, const char *module_output)
{
    if (!agg || !module_output)
    {
        return;
    }

    gsize len = strlen(module_output);
    const char *buf = module_output;
    gsize pos = 0;

    while (pos < len)
    {
        /* 找到下一个 SOH; SOH 之前的内容一律作为全局片段 */
        gsize soh = find_delim(buf, len, pos);
        if (soh > pos)
        {
            g_string_append_len(agg->global, buf + pos, soh - pos);
            pos = soh;
        }
        if (pos >= len)
        {
            break;
        }

        /* 解析标志头 */
        char kind = 0;
        char key[CLI_CFG_ANCHOR_KEY_MAX];
        gsize payload_start = try_parse_header(buf, len, pos, &kind, key, sizeof(key));
        if (payload_start == 0)
        {
            /* 不是合法标志头: 把这个 SOH 原样计入全局(不应该发生, 但容错) */
            LOG_WARN("cli_cfg_anchor: stray SOH at pos=%zu, preserving as global", (size_t)pos);
            g_string_append_c(agg->global, buf[pos]);
            pos++;
            continue;
        }

        /* 找结束标志 */
        gsize end_marker = 0;
        gsize payload_bytes = 0;
        if (!find_end_marker(buf, len, payload_start, &end_marker, &payload_bytes))
        {
            /* 缺失结束标志: 丢弃剩余内容, 记录警告 */
            LOG_WARN("cli_cfg_anchor: missing END marker after kind='%c' key='%s'", kind, key);
            break;
        }

        apply_payload_to_bucket(agg, kind, key, buf + payload_start, payload_bytes);
        pos = end_marker + 4; /* 跳过 \x01 E \x01 \n */
    }
}

void cli_cfg_anchor_agg_render(cli_cfg_anchor_aggregator_t *agg, GString *out)
{
    if (!agg || !out)
    {
        return;
    }

    /* 1) 先输出全局片段 */
    if (agg->global && agg->global->len > 0)
    {
        g_string_append_len(out, agg->global->str, agg->global->len);
    }

    /* 2) 按首次出现顺序输出每个 anchor */
    for (GList *node = agg->order_keys; node; node = node->next)
    {
        const char *key = (const char *)node->data;
        anchor_bucket_t *bucket = g_hash_table_lookup(agg->anchors, key);
        if (!bucket)
        {
            continue;
        }

        if (!bucket->header_set)
        {
            /* 孤儿: 有贡献但无属主段头 —— 丢弃, 记录警告 */
            LOG_WARN("cli_cfg_anchor: orphan contributions for key='%s' dropped (no owner header)", key);
            continue;
        }

        /* 没有任何贡献时整体跳过, 避免输出 "header + 立即 footer" 的空壳 */
        if (!bucket->body || bucket->body->len == 0)
        {
            continue;
        }

        g_string_append_len(out, bucket->header->str, bucket->header->len);
        g_string_append_len(out, bucket->body->str, bucket->body->len);

        if (bucket->footer_set && bucket->footer->len > 0)
        {
            g_string_append_len(out, bucket->footer->str, bucket->footer->len);
        }
        else
        {
            g_string_append(out, CLI_CFG_ANCHOR_DEFAULT_FOOTER);
        }
    }
}
