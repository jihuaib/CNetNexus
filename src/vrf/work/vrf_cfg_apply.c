/**
 * @file   vrf_cfg_apply.c
 * @brief  VRF 配置应用实现（worker 线程内：内存表 + OS + 事件，**不写 DB**）
 * @author jhb
 * @date   2026/05/02
 *
 * @note DB 持久化由 CLI 路径在 dispatch_apply 返回后处理；恢复路径已有 DB 数据，
 *       直接 dispatch 即可，apply 函数对两种路径行为一致。
 */
#include "vrf_cfg_apply.h"

#include <stdio.h>
#include <string.h>

#include "log.h"
#include "vrf_os.h"
#include "vrf_pub.h"
#include "vrf_table.h"
#include "vrf_worker.h"

static int apply_vrf_create(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *existing = vrf_table_find_by_name(t, cmd->vrf_name);
    if (existing)
    {
        cmd->vrf_id = existing->vrf_id;
        cmd->l3vrf_table_id = existing->l3vrf_table_id;
        return 0; /* 幂等 */
    }

    /* 恢复路径用 DB 里的 vrf_id/l3vrf_table_id；CLI 路径（vrf_id=0）让 table 自分配 */
    vrf_entry_t *e = (cmd->vrf_id != 0) ? vrf_table_create_with_id(t, cmd->vrf_id, cmd->vrf_name, cmd->l3vrf_table_id)
                                        : vrf_table_create(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
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

    /* 回传给调用方做 DB 写 */
    cmd->vrf_id = e->vrf_id;
    cmd->l3vrf_table_id = e->l3vrf_table_id;
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

    (void)vrf_os_remove(e->name);
    cmd->vrf_id = vrf_id; /* 回传给调用方做 DB 删除 */
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
    cmd->vrf_id = e->vrf_id; /* 回传 */
    if (vrf_af_find(e, cmd->afi, cmd->safi))
    {
        return VRF_APPLY_RC_NOOP;
    }
    vrf_af_state_t *af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
    if (!af)
    {
        return VRF_APPLY_RC_FAIL;
    }
    vrf_pub_notify_af_enable(e, cmd->afi, cmd->safi);
    return VRF_APPLY_RC_OK;
}

static int apply_af_delete(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e)
    {
        return -1;
    }
    cmd->vrf_id = e->vrf_id; /* 回传 */
    if (!vrf_af_find(e, cmd->afi, cmd->safi))
    {
        return VRF_APPLY_RC_NOOP;
    }
    vrf_pub_notify_af_disable(e, cmd->afi, cmd->safi);
    return vrf_af_delete(e, cmd->afi, cmd->safi);
}

static int apply_rd_set(vrf_apply_cmd_t *cmd, int clear)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e || e->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        snprintf(cmd->errmsg, sizeof(cmd->errmsg), "VRF Error: VRF not found\r\n");
        return VRF_APPLY_RC_FAIL;
    }
    cmd->vrf_id = e->vrf_id; /* 回传 */
    vrf_af_state_t *af = vrf_af_find(e, cmd->afi, cmd->safi);
    if (clear)
    {
        if (!af || !af->has_rd)
        {
            return VRF_APPLY_RC_NOOP;
        }
    }
    else
    {
        if (af && af->has_rd)
        {
            snprintf(cmd->errmsg, sizeof(cmd->errmsg), "VRF Error: RD already configured; delete it first\r\n");
            return VRF_APPLY_RC_FAIL;
        }
        if (!af)
        {
            af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
            if (!af)
            {
                snprintf(cmd->errmsg, sizeof(cmd->errmsg), "VRF Error: AF create failed\r\n");
                return VRF_APPLY_RC_FAIL;
            }
        }
    }
    int rc = vrf_af_set_rd(af, clear ? NULL : &cmd->rd);
    if (rc != 0)
    {
        return VRF_APPLY_RC_FAIL;
    }
    if (clear)
    {
        vrf_pub_notify_af_rd_del(e, cmd->afi, cmd->safi);
    }
    else
    {
        vrf_pub_notify_af_rd_add(e, af);
    }
    return VRF_APPLY_RC_OK;
}

static int apply_apply_label_set(vrf_apply_cmd_t *cmd)
{
    vrf_table_t *t = vrf_worker_table();
    vrf_entry_t *e = vrf_table_find_by_name(t, cmd->vrf_name);
    if (!e || e->vrf_id == VRF_PUBLIC_VRF_ID)
    {
        snprintf(cmd->errmsg, sizeof(cmd->errmsg), "VRF Error: VRF not found\r\n");
        return VRF_APPLY_RC_FAIL;
    }
    cmd->vrf_id = e->vrf_id; /* 回传 */
    vrf_af_state_t *af = vrf_af_get_or_create(e, cmd->afi, cmd->safi);
    if (!af)
    {
        snprintf(cmd->errmsg, sizeof(cmd->errmsg), "VRF Error: AF create failed\r\n");
        return VRF_APPLY_RC_FAIL;
    }
    if (af->apply_label_mode == cmd->apply_label_mode)
    {
        return VRF_APPLY_RC_NOOP;
    }
    af->apply_label_mode = cmd->apply_label_mode;
    vrf_pub_notify_af_apply_label(e, af);
    return VRF_APPLY_RC_OK;
}

static gboolean rt_array_contains(const GArray *arr, const vrf_rt_t *rt)
{
    if (!arr || !rt)
    {
        return FALSE;
    }
    for (guint i = 0; i < arr->len; i++)
    {
        const vrf_rt_t *cur = &g_array_index(arr, vrf_rt_t, i);
        if (memcmp(cur->bytes, rt->bytes, sizeof(rt->bytes)) == 0)
        {
            return TRUE;
        }
    }
    return FALSE;
}

static int apply_rt_modify_dir(vrf_entry_t *e, vrf_af_state_t *af, vrf_apply_cmd_t *cmd, int direction)
{
    GArray *arr = (direction == 0) ? af->import_rts : af->export_rts;
    gboolean existed = rt_array_contains(arr, &cmd->rt);
    if (vrf_af_modify_rt(af, direction, cmd->add ? 1 : 0, &cmd->rt) != 0)
    {
        return -1;
    }
    if ((cmd->add && existed) || (!cmd->add && !existed))
    {
        return 0;
    }
    if (direction == 0)
    {
        if (cmd->add)
        {
            vrf_pub_notify_af_import_rt_add(e, af, &cmd->rt);
        }
        else
        {
            vrf_pub_notify_af_import_rt_del(e, af, &cmd->rt);
        }
    }
    else
    {
        if (cmd->add)
        {
            vrf_pub_notify_af_export_rt_add(e, af, &cmd->rt);
        }
        else
        {
            vrf_pub_notify_af_export_rt_del(e, af, &cmd->rt);
        }
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
    cmd->vrf_id = e->vrf_id; /* 回传 */
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
        case VRF_APPLY_OP_APPLY_LABEL_SET:
            return apply_apply_label_set(cmd);
        default:
            return -1;
    }
}
