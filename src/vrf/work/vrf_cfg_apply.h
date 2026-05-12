/**
 * @file   vrf_cfg_apply.h
 * @brief  VRF 配置应用（worker 线程）：内存表 + DB 持久化 + 事件发布
 * @author jhb
 * @date   2026/05/02
 */
#ifndef VRF_CFG_APPLY_H
#define VRF_CFG_APPLY_H

#include <stdint.h>

#include "vrf.h"

/**
 * @brief 配置应用操作类型
 */
typedef enum vrf_apply_op
{
    VRF_APPLY_OP_VRF_CREATE = 1,
    VRF_APPLY_OP_VRF_DELETE = 2,
    VRF_APPLY_OP_AF_CREATE = 3,
    VRF_APPLY_OP_AF_DELETE = 4,
    VRF_APPLY_OP_RD_SET = 5, /**< rd 字段有效 */
    VRF_APPLY_OP_RD_CLEAR = 6,
    VRF_APPLY_OP_RT_MODIFY = 7, /**< direction + add + rt 有效 */
} vrf_apply_op_t;

/**
 * @brief 配置应用命令载荷（IPC 线程构造，worker 线程执行）
 *
 * @note worker 只负责内存表 + OS 下发 + 事件发布，**不写 DB**。
 *       DB 持久化由 CLI/恢复路径在 dispatch_apply 返回后自行处理；
 *       worker 通过 vrf_id / l3vrf_table_id 字段把分配/查到的标识回传给调用方。
 */
typedef struct vrf_apply_cmd
{
    vrf_apply_op_t op;
    int rc;

    char vrf_name[VRF_NAME_MAX_LEN];
    uint16_t afi;
    uint8_t safi;
    uint8_t direction; /**< 0=import, 1=export, 2=both */
    uint8_t add;       /**< 1=添加, 0=删除（RT_MODIFY 时） */
    uint8_t _pad[3];

    /** 输入/输出：VRF_CREATE 输入 0 表示 worker 分配，非 0 表示按指定 id 建（恢复路径用）；
     *  其它操作或 CREATE 完成后由 worker 回填实际 vrf_id 给调用方做 DB 写。 */
    uint32_t vrf_id;
    /** 输入/输出：同 vrf_id。 */
    uint32_t l3vrf_table_id;

    vrf_rd_t rd;
    vrf_rt_t rt;
} vrf_apply_cmd_t;

/**
 * @brief 在 worker 线程中执行配置应用
 * @return 0 成功，-1 失败
 */
int vrf_cfg_apply(vrf_apply_cmd_t *cmd);

#endif /* VRF_CFG_APPLY_H */
