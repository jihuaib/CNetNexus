/**
 * @file   if_cfg_apply.c
 * @brief  接口配置内存态应用实现（CLI / DB 恢复共用）
 * @author jhb
 * @date   2026/03/08
 */
#include "if_cfg_apply.h"

#include <arpa/inet.h>
#include <string.h>

#include "errcode.h"
#include "if.h"
#include "if_main.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 公共 API
// ============================================================================

if_map_entry_t *if_cfg_find_entry(const char *logical_name)
{
    if (!logical_name || !g_if_local)
    {
        return NULL;
    }

    if_map_t *map = &g_if_local->interface_map;
    for (int i = 0; i < map->count; i++)
    {
        if (strcmp(map->entries[i].logical_name, logical_name) == 0)
        {
            return &map->entries[i];
        }
    }

    return NULL;
}

int if_cfg_apply_ip(gboolean is_no, const char *logical_name, const net_prefix_t *prefix)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: 未找到接口 %s", logical_name);
        return ERRCODE_FAIL;
    }

    if (is_no)
    {
        memset(&entry->prefix, 0, sizeof(entry->prefix));
        if_set_ip(entry->physical_name, "0.0.0.0", "0.0.0.0");
        LOG_INFO("IF: %s IP 已清除", logical_name);
        return ERRCODE_SUCCESS;
    }

    if (!prefix || !net_prefix_is_set(prefix))
    {
        return ERRCODE_FAIL;
    }

    /* 转字符串用于 ioctl */
    char ip_str[64];
    net_addr_to_str(&prefix->addr, ip_str, sizeof(ip_str));

    char mask_str[16];
    net_prefix_len_to_mask_str(prefix->prefix_len, mask_str);

    if (if_set_ip(entry->physical_name, ip_str, mask_str) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: 配置物理接口 %s IP 失败", entry->physical_name);
        return ERRCODE_FAIL;
    }

    entry->prefix = *prefix;
    LOG_INFO("IF: %s IP=%s/%u 已配置", logical_name, ip_str, prefix->prefix_len);
    return ERRCODE_SUCCESS;
}

int if_cfg_apply_shutdown(gboolean is_no, const char *logical_name)
{
    if (!logical_name)
    {
        return ERRCODE_FAIL;
    }

    if_map_entry_t *entry = if_cfg_find_entry(logical_name);
    if (!entry)
    {
        LOG_ERROR("IF: 未找到接口 %s", logical_name);
        return ERRCODE_FAIL;
    }

    /* is_no=TRUE → no shutdown → up；is_no=FALSE → shutdown → down */
    int up = is_no ? 1 : 0;

    if (if_set_state(entry->physical_name, up) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("IF: 设置接口 %s 状态失败", entry->physical_name);
        return ERRCODE_FAIL;
    }

    entry->shutdown = up ? 0 : 1;
    LOG_INFO("IF: %s %s", logical_name, up ? "no shutdown" : "shutdown");
    return ERRCODE_SUCCESS;
}
