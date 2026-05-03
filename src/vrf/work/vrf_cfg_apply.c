/**
 * @file   vrf_cfg_apply.c
 * @brief  VRF 配置应用实现（worker 线程内：内存表 + DB + 事件）
 * @author jhb
 * @date   2026/05/02
 */
#include "vrf_cfg_apply.h"

#include <string.h>

#include "log.h"
#include "vrf_db.h"
#include "vrf_os.h"
#include "vrf_pub.h"
#include "vrf_table.h"
#include "vrf_worker.h"

static int apply_vrf_create(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    if (vrf_table_find_by_name(t, cmd->vrf_name))
    {
        return 0; /* 幂等 */
    }
    vrf_entry_t *e = vrf_table_create(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
    }
    if (vrf_db_insert_vrf(e->vrf_id, e->name, e->l3vrf_table_id) != 0)
    {
        LOG_WARN("VRF: DB insert failed for vrf=%s", e->name);
    }

    /* 下发 OS L3VRF 设备；公网 vrf_id=0 不进入此分支（CLI 已挡） */
    if (vrf_os_install(e->name, e->l3vrf_table_id) == 0)
    {
        e->os_state = VRF_OS_STATE_UP;
    }
    else
    {
        e->os_state = VRF_OS_STATE_DOWN;
        LOG_WARN("VRF: OS install failed for vrf=%s table=%u", e->name, e->l3vrf_table_id);
    }

    vrf_pub_notify_vrf_add(e);
    vrf_pub_notify_vrf_state(e);
    return 0;
}

static int apply_vrf_delete(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
    }
    if (e->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return -1;
    }
    uint32_t vrf_id = e->vrf_id;

    /* 先发事件，订阅者拿到 entry 上的 name/id */
    vrf_pub_notify_vrf_del(e);

    (void)vrf_db_delete_vrf(vrf_id);
    (void)vrf_os_remove(e->name);
    return vrf_table_delete(t, vrf_id);
}

static int apply_af_create(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
    }
    if (vrf_af_find(e, cmd->afi, cmd->safi))
    {
        return 0;
    }
    vrf_af_state_t *af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
    if (!af)
    {
        return -1;
    }
    (void)vrf_db_set_af_rd(e->vrf_id, cmd->afi, cmd->safi, NULL);
    vrf_pub_notify_af_enable(e, cmd->afi, cmd->safi);
    return 0;
}

static int apply_af_delete(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
    }
    if (!vrf_af_find(e, cmd->afi, cmd->safi))
    {
        return 0;
    }
    vrf_pub_notify_af_disable(e, cmd->afi, cmd->safi);
    (void)vrf_db_delete_af(e->vrf_id, cmd->afi, cmd->safi);
    return vrf_af_delete(e, cmd->afi, cmd->safi);
}

static int apply_rd_set(vrf_apply_cmd_t *cmd, int clear)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e || e->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return -1;
    }
    vrf_af_state_t *af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
    if (!af)
    {
        return -1;
    }
    int rc = vrf_af_set_rd(af, clear ? NULL : &cmd->rd);
    if (rc != 0)
    {
        return -1;
    }
    (void)vrf_db_set_af_rd(e->vrf_id, cmd->afi, cmd->safi, clear ? NULL : &cmd->rd);
    vrf_pub_notify_af_rd(e, af);
    return 0;
}

static int apply_rt_modify_dir(vrf_entry_t *e, vrf_af_state_t *af, vrf_apply_cmd_t *cmd, int direction)
{
    if (vrf_af_modify_rt(af, direction, cmd->add ? 1 : 0, &cmd->rt) != 0)
    {
        return -1;
    }
    (void)vrf_db_modify_rt(e->vrf_id, cmd->afi, cmd->safi, direction, cmd->add ? 1 : 0, &cmd->rt);
    if (direction == 0)
    {
        vrf_pub_notify_af_import_rt(e, af);
    }
    else
    {
        vrf_pub_notify_af_export_rt(e, af);
    }
    return 0;
}

static int apply_rt_modify(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e || e->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        return -1;
    }
    vrf_af_state_t *af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
    if (!af)
    {
        return -1;
    }
    int rc = 0;
    if (cmd->direction == 0 || cmd->direction == 2)
    {
        rc |= apply_rt_modify_dir(e, af, cmd, 0);
    }
    if (cmd->direction == 1 || cmd->direction == 2)
    {
        rc |= apply_rt_modify_dir(e, af, cmd, 1);
    }
    return rc;
}

int vrf_cfg_apply(vrf_apply_cmd_t *cmd)
{
    if (!cmd)
    {
        return -1;
    }
    switch (cmd->op)
    {
        case VRF_APPLY_OP_VRF_CREATE:
            return apply_vrf_create(cmd);
        case VRF_APPLY_OP_VRF_DELETE:
            return apply_vrf_delete(cmd);
        case VRF_APPLY_OP_AF_CREATE:
            return apply_af_create(cmd);
        case VRF_APPLY_OP_AF_DELETE:
            return apply_af_delete(cmd);
        case VRF_APPLY_OP_RD_SET:
            return apply_rd_set(cmd, 0);
        case VRF_APPLY_OP_RD_CLEAR:
            return apply_rd_set(cmd, 1);
        case VRF_APPLY_OP_RT_MODIFY:
            return apply_rt_modify(cmd);
        default:
            return -1;
    }
}
