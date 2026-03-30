/**
 * @file   route_work.c
 * @brief  Route 工作事件处理实现
 * @author jhb
 * @date   2026/03/28
 */
#include "route_work.h"

#include "log.h"
#include "route_calc.h"
#include "route_main.h"
#include "route_relay.h"
#include "route_rib.h"
#include "route_worker.h"
void route_work_handle_calc_event(const route_head_key_t *key)
{
    if (!key || !g_route_work_local || !g_route_work_local->rib)
    {
        return;
    }

    const route_head_t *head =
        route_rib_lookup_head(g_route_work_local->rib, key->vrf_id, key->afi, &key->addr, key->prefix_len);
    if (!head)
    {
        return;
    }

    route_calc_on_path_add(head);
    route_recompute_iter_paths();
}
