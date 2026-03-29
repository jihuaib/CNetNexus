/**
 * @file   bgp_bmp_cfg_apply.c
 * @brief  BGP BMP 配置应用实现（worker 线程内执行）
 * @author jhb
 * @date   2026/03/29
 */
#include "bgp_bmp_cfg_apply.h"

#include <string.h>

#include "bgp_bmp.h"
#include "bgp_worker.h"
#include "log.h"
#include "net_addr.h"

// ============================================================================
// 辅助
// ============================================================================

/**
 * @brief 查找 BMP 实例（从全局哈希表）
 */
static bgp_bmp_instance_t *find_bmp_inst(const char *name)
{
    if (!g_bgp_work_local->bmp_instances)
    {
        return NULL;
    }
    return (bgp_bmp_instance_t *)g_hash_table_lookup(g_bgp_work_local->bmp_instances, name);
}

/**
 * @brief 确保 bmp_instances 哈希表已创建
 */
static void ensure_bmp_hash(void)
{
    if (!g_bgp_work_local->bmp_instances)
    {
        g_bgp_work_local->bmp_instances = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
}

// ============================================================================
// bmp instance / no bmp instance
// ============================================================================

void bgp_bmp_cfg_apply_instance(bgp_apply_cmd_t *apply)
{
    const char *name = apply->u.bmp_instance.name;

    if (apply->isNo)
    {
        /* 删除 BMP 实例 */
        bgp_bmp_instance_t *inst = find_bmp_inst(name);
        if (!inst)
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP instance '%s' not found.", name);
            apply->rc = BGP_APPLY_RC_FAIL;
            return;
        }

        bgp_bmp_instance_destroy(inst, g_bgp_work_local->epoll_fd);
        g_hash_table_remove(g_bgp_work_local->bmp_instances, name);
        apply->rc = BGP_APPLY_RC_OK;
        LOG_INFO("BMP instance '%s' destroyed", name);
        return;
    }

    /* 创建 BMP 实例（幂等） */
    bgp_bmp_instance_t *existing = find_bmp_inst(name);
    if (existing)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    ensure_bmp_hash();
    bgp_bmp_instance_t *inst = bgp_bmp_instance_create(name);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP: Failed to create instance '%s'.", name);
        apply->rc = BGP_APPLY_RC_FAIL;
        return;
    }

    g_hash_table_insert(g_bgp_work_local->bmp_instances, g_strdup(name), inst);
    apply->rc = BGP_APPLY_RC_OK;
    LOG_INFO("BMP instance '%s' created", name);
}

// ============================================================================
// collector / no collector
// ============================================================================

void bgp_bmp_cfg_apply_collector(bgp_apply_cmd_t *apply)
{
    const char *inst_name = apply->u.bmp_collector.inst_name;
    bgp_bmp_instance_t *inst = find_bmp_inst(inst_name);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP instance '%s' not found.", inst_name);
        apply->rc = BGP_APPLY_RC_FAIL;
        return;
    }

    if (apply->isNo)
    {
        /* 清除 collector 配置，断开连接 */
        if (inst->collector_port == 0)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
        bgp_bmp_disconnect(inst, g_bgp_work_local->epoll_fd);
        memset(&inst->collector_addr, 0, sizeof(inst->collector_addr));
        inst->collector_port = 0;
        inst->conn_state = BGP_BMP_CONN_IDLE;
        apply->rc = BGP_APPLY_RC_OK;
        LOG_INFO("BMP instance '%s' collector cleared", inst_name);
        return;
    }

    /* 设置 collector 地址和端口 */
    net_addr_t new_addr = apply->u.bmp_collector.addr;
    uint16_t new_port = apply->u.bmp_collector.port;

    /* 检查是否有变化 */
    if (net_addr_equal(&inst->collector_addr, &new_addr) && inst->collector_port == new_port)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    /* 如果已有连接，先断开 */
    if (inst->conn_state != BGP_BMP_CONN_IDLE)
    {
        bgp_bmp_disconnect(inst, g_bgp_work_local->epoll_fd);
    }

    inst->collector_addr = new_addr;
    inst->collector_port = new_port;

    /* 有地址和端口后立即发起连接 */
    if (new_port > 0)
    {
        bgp_bmp_connect(inst, g_bgp_work_local->epoll_fd);
    }

    apply->rc = BGP_APPLY_RC_OK;
    LOG_INFO("BMP instance '%s' collector set to port %u", inst_name, new_port);
}

// ============================================================================
// stats-report interval / no stats-report interval
// ============================================================================

void bgp_bmp_cfg_apply_stats(bgp_apply_cmd_t *apply)
{
    const char *inst_name = apply->u.bmp_stats.inst_name;
    bgp_bmp_instance_t *inst = find_bmp_inst(inst_name);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP instance '%s' not found.", inst_name);
        apply->rc = BGP_APPLY_RC_FAIL;
        return;
    }

    uint16_t new_interval = apply->isNo ? 0 : apply->u.bmp_stats.interval;
    if (inst->stats_interval == new_interval)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    inst->stats_interval = new_interval;
    /* TODO Phase 7: 重新调度 stats 定时器 */

    apply->rc = BGP_APPLY_RC_OK;
    LOG_INFO("BMP instance '%s' stats interval set to %u", inst_name, new_interval);
}

// ============================================================================
// reconnect interval / no reconnect interval
// ============================================================================

void bgp_bmp_cfg_apply_reconnect(bgp_apply_cmd_t *apply)
{
    const char *inst_name = apply->u.bmp_reconnect.inst_name;
    bgp_bmp_instance_t *inst = find_bmp_inst(inst_name);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP instance '%s' not found.", inst_name);
        apply->rc = BGP_APPLY_RC_FAIL;
        return;
    }

    uint16_t new_interval = apply->isNo ? 30 : apply->u.bmp_reconnect.interval;
    if (inst->reconnect_interval == new_interval)
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    inst->reconnect_interval = new_interval;
    apply->rc = BGP_APPLY_RC_OK;
    LOG_INFO("BMP instance '%s' reconnect interval set to %u", inst_name, new_interval);
}

// ============================================================================
// monitor neighbor 配置
// ============================================================================

void bgp_bmp_cfg_apply_monitor(bgp_apply_cmd_t *apply)
{
    const char *inst_name = apply->u.bmp_monitor.inst_name;
    bgp_bmp_instance_t *inst = find_bmp_inst(inst_name);
    if (!inst)
    {
        snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP instance '%s' not found.", inst_name);
        apply->rc = BGP_APPLY_RC_FAIL;
        return;
    }

    if (apply->u.bmp_monitor.is_all && !apply->isNo)
    {
        /* monitor neighbor all */
        if (inst->monitor_all)
        {
            apply->rc = BGP_APPLY_RC_NOOP;
            return;
        }
        inst->monitor_all = TRUE;
        if (inst->monitor_peers)
        {
            g_hash_table_remove_all(inst->monitor_peers);
        }
        apply->rc = BGP_APPLY_RC_OK;
        LOG_INFO("BMP instance '%s' monitor set to all", inst_name);
        return;
    }

    /* monitor neighbor <ip> / no monitor neighbor <ip> */
    char ip_str[64];
    net_addr_to_str(&apply->u.bmp_monitor.addr, ip_str, sizeof(ip_str));

    if (apply->isNo)
    {
        /* no monitor neighbor <ip> */
        if (!inst->monitor_peers || !g_hash_table_remove(inst->monitor_peers, ip_str))
        {
            snprintf(apply->errmsg, sizeof(apply->errmsg), "BMP: Monitor peer '%s' not found.", ip_str);
            apply->rc = BGP_APPLY_RC_FAIL;
            return;
        }
        apply->rc = BGP_APPLY_RC_OK;
        LOG_INFO("BMP instance '%s' removed monitor peer %s", inst_name, ip_str);
        return;
    }

    /* monitor neighbor <ip>：切换到非 all 模式 */
    inst->monitor_all = FALSE;
    if (!inst->monitor_peers)
    {
        inst->monitor_peers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }

    if (g_hash_table_contains(inst->monitor_peers, ip_str))
    {
        apply->rc = BGP_APPLY_RC_NOOP;
        return;
    }

    g_hash_table_insert(inst->monitor_peers, g_strdup(ip_str), NULL);
    apply->rc = BGP_APPLY_RC_OK;
    LOG_INFO("BMP instance '%s' added monitor peer %s", inst_name, ip_str);
}
