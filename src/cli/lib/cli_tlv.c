/**
 * @file   cli_tlv.c
 * @brief  CLI TLV 载荷解析工具函数
 * @author jhb
 * @date   2026/03/08
 */

#include <arpa/inet.h>
#include <glib.h>
#include <string.h>

#include "cli.h"
#include "db.h"

/* ========================================================================= */
/* TLV 载荷解析器实现                                                         */
/* ========================================================================= */

int cli_tlv_init(cli_tlv_parser_t *p, const uint8_t *data, uint32_t len)
{
    if (!p || !data || len < 5)
    {
        return -1;
    }

    memset(p, 0, sizeof(*p));
    p->_data = data;
    p->_len = len;
    p->_pos = 0;

    /* flags: u8 */
    p->flags = data[p->_pos++];

    /* group_id: u32 */
    if (p->_pos + 4 > p->_len)
    {
        return -1;
    }
    uint32_t be;
    memcpy(&be, data + p->_pos, 4);
    p->group_id = ntohl(be);
    p->_pos += 4;

    return 0;
}

int cli_tlv_next(cli_tlv_parser_t *p, cli_tlv_entry_t *entry)
{
    if (!p || !entry)
    {
        return -1;
    }

    if (p->_pos >= p->_len)
    {
        return 0;
    }

    /* cfg_id: u32 */
    if (p->_pos + 4 > p->_len)
    {
        return 0;
    }
    uint32_t id_be;
    memcpy(&id_be, p->_data + p->_pos, 4);
    entry->cfg_id = ntohl(id_be);
    p->_pos += 4;

    /* type: u8 */
    if (p->_pos >= p->_len)
    {
        return -1;
    }
    entry->type = p->_data[p->_pos++];

    /* length: u16 */
    if (p->_pos + 2 > p->_len)
    {
        return -1;
    }
    uint16_t len_be;
    memcpy(&len_be, p->_data + p->_pos, 2);
    entry->length = ntohs(len_be);
    p->_pos += 2;

    /* value: bytes（多分配 1 字节用于 NUL 终止，方便文本读取） */
    if (entry->length > 0)
    {
        if (p->_pos + entry->length > p->_len)
        {
            return -1;
        }
        entry->value = g_malloc(entry->length + 1);
        memcpy(entry->value, p->_data + p->_pos, entry->length);
        entry->value[entry->length] = '\0';
        p->_pos += entry->length;
    }
    else
    {
        entry->value = NULL;
    }

    return 1;
}

void cli_tlv_entry_free(cli_tlv_entry_t *entry)
{
    if (entry)
    {
        g_free(entry->value);
        entry->value = NULL;
    }
}

void cli_tlv_cleanup(cli_tlv_parser_t *p)
{
    if (p)
    {
        memset(p, 0, sizeof(*p));
    }
}

int64_t cli_tlv_entry_get_int(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != DB_TYPE_INTEGER || entry->length != 8 || !entry->value)
    {
        return 0;
    }
    uint32_t hi, lo;
    memcpy(&hi, entry->value, 4);
    memcpy(&lo, entry->value + 4, 4);
    hi = ntohl(hi);
    lo = ntohl(lo);
    return ((int64_t)hi << 32) | lo;
}

uint32_t cli_tlv_entry_get_ctx_uint32(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != CLI_TLV_TYPE_CTX || entry->length != 4 || !entry->value)
    {
        return 0;
    }
    uint32_t v;
    memcpy(&v, entry->value, 4);
    return ntohl(v);
}

const char *cli_tlv_entry_get_text(const cli_tlv_entry_t *entry)
{
    if (!entry || entry->type != DB_TYPE_TEXT || !entry->value)
    {
        return NULL;
    }
    /* value 在 tlv_next 中多分配了 1 字节并追加了 NUL */
    return (const char *)entry->value;
}
