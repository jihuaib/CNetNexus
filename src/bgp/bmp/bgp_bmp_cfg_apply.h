/**
 * @file   bgp_bmp_cfg_apply.h
 * @brief  BMP 配置应用接口（BMP 线程内执行）
 * @author jhb
 * @date   2026/03/29
 */
#ifndef BGP_BMP_CFG_APPLY_H
#define BGP_BMP_CFG_APPLY_H

/* 前向声明 */
struct bgp_apply_cmd;
typedef struct bgp_apply_cmd bgp_apply_cmd_t;

/**
 * @brief 应用 bmp instance/no bmp instance 到内存
 */
void bgp_bmp_cfg_apply_instance(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 collector/no collector 到内存
 */
void bgp_bmp_cfg_apply_collector(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 stats-report interval/no stats-report interval 到内存
 */
void bgp_bmp_cfg_apply_stats(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 reconnect interval/no reconnect interval 到内存
 */
void bgp_bmp_cfg_apply_reconnect(bgp_apply_cmd_t *apply);

/**
 * @brief 应用 monitor neighbor 配置到内存
 */
void bgp_bmp_cfg_apply_monitor(bgp_apply_cmd_t *apply);

#endif /* BGP_BMP_CFG_APPLY_H */
