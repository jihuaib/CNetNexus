/**
 * @file   bgp_parse_af.c
 * @brief  AFI/SAFI 处理器注册表
 * @author jhb
 * @date   2026/03/11
 */
#include <stdint.h>
#include <string.h>

#include "bgp_parse.h"

/** 最大注册处理器数 */
#define BGP_AF_PARSER_MAX 32

static const bgp_af_parser_t *g_parsers[BGP_AF_PARSER_MAX];
static int g_parser_count = 0;

int bgp_af_parser_register(const bgp_af_parser_t *parser)
{
    if (!parser)
    {
        return -1;
    }

    /* 已存在相同 AFI/SAFI 则覆盖 */
    for (int i = 0; i < g_parser_count; i++)
    {
        if (g_parsers[i]->afi == parser->afi && g_parsers[i]->safi == parser->safi)
        {
            g_parsers[i] = parser;
            return 0;
        }
    }

    if (g_parser_count >= BGP_AF_PARSER_MAX)
    {
        return -1;
    }

    g_parsers[g_parser_count++] = parser;
    return 0;
}

const bgp_af_parser_t *bgp_af_parser_find(uint16_t afi, uint8_t safi)
{
    for (int i = 0; i < g_parser_count; i++)
    {
        if (g_parsers[i]->afi == afi && g_parsers[i]->safi == safi)
        {
            return g_parsers[i];
        }
    }
    return NULL;
}
