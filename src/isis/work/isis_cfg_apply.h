/**
 * @file   isis_cfg_apply.h
 * @brief  ISIS 配置内存态应用接口（校验 + 短路 + 内存更新 + 副作用，结果写入 apply->rc/errmsg）
 * @author jhb
 * @date   2026/05/16
 */
#ifndef ISIS_CFG_APPLY_H
#define ISIS_CFG_APPLY_H

struct isis_apply_cmd;
typedef struct isis_apply_cmd isis_apply_cmd_t;

/** 创建/更新 instance（net, is_type, admin_up） */
void isis_cfg_apply_instance_set(isis_apply_cmd_t *apply);
/** 删除 instance */
void isis_cfg_apply_instance_del(isis_apply_cmd_t *apply);
/** 设置 NET（system-id） */
void isis_cfg_apply_net_set(isis_apply_cmd_t *apply);
/** 设置 is-type（level-1/2/1-2） */
void isis_cfg_apply_is_type_set(isis_apply_cmd_t *apply);
/** 启用 AF */
void isis_cfg_apply_af_set(isis_apply_cmd_t *apply);
/** 禁用 AF */
void isis_cfg_apply_af_del(isis_apply_cmd_t *apply);
/** 接口配置 set（enable/metric/hello-interval/hold-multiplier/passive 等） */
void isis_cfg_apply_if_set(isis_apply_cmd_t *apply);
/** 接口配置 del */
void isis_cfg_apply_if_del(isis_apply_cmd_t *apply);
/** 切换 cost-style（narrow/wide） */
void isis_cfg_apply_cost_style_set(isis_apply_cmd_t *apply);

#endif /* ISIS_CFG_APPLY_H */
