/**
 * @file config_capture.h
 * @brief 配置快照必须覆盖的模块及按需模块配置存在标识
 */
#ifndef CONFIG_CAPTURE_H
#define CONFIG_CAPTURE_H

#include <glib.h>
#include <stdint.h>

#include "dev.h"

typedef struct config_capture_owner
{
    uint32_t module_id;
    const char *module_name;
    gboolean always_required;
    const char *revive_table;
} config_capture_owner_t;

/*
 * 常驻模块只要缺席就不能声称捕获完整；按需模块仅在 revive_table 指定的
 * 任一配置表有行时必须在线。逗号分隔表清单与各模块 module.conf 保持一致。
 */
static const config_capture_owner_t CONFIG_CAPTURE_OWNERS[] = {
    {DEV_MODULE_ID_DEV, "dev", TRUE, NULL},
    {DEV_MODULE_ID_VRF, "vrf", TRUE, NULL},
    {DEV_MODULE_ID_IF, "if", TRUE, NULL},
    {DEV_MODULE_ID_ROUTE, "route", TRUE, NULL},
    {DEV_MODULE_ID_ACCESS, "access", TRUE, NULL},
    {DEV_MODULE_ID_RPM, "rpm", FALSE, "rpm_policy"},
    /* BGP/ISIS 的 SRv6 locator 引用必须在 locator 本体之后回放。 */
    {DEV_MODULE_ID_SRV6, "srv6", FALSE, "srv6_locator"},
    {DEV_MODULE_ID_BGP, "bgp", FALSE, "bgp_protocol"},
    {DEV_MODULE_ID_SBMP, "sbmp", FALSE, "sbmp_server"},
    {DEV_MODULE_ID_ISIS, "isis", FALSE, "isis_instance"},
    {DEV_MODULE_ID_LDP, "ldp", FALSE, "ldp_protocol,ldp_interface"},
    {DEV_MODULE_ID_LLDP, "lldp", FALSE, "lldp_protocol,lldp_interface"},
    {DEV_MODULE_ID_SNMP, "snmp", FALSE, "snmp_config"},
    {DEV_MODULE_ID_OSPF, "ospf", FALSE, "ospf_instance"},
    {DEV_MODULE_ID_OSPFV3, "ospfv3", FALSE, "ospfv3_instance"},
};

#define CONFIG_CAPTURE_OWNER_COUNT G_N_ELEMENTS(CONFIG_CAPTURE_OWNERS)

#endif /* CONFIG_CAPTURE_H */
