/**
 * @file   telnet_access.h
 * @brief  Telnet接入模块
 * @author jhb
 * @date   2026/02/07
 */
#ifndef TELNET_ACCESS_H
#define TELNET_ACCESS_H

#include "access_module.h"

#ifdef __cplusplus
extern "C" {
#endif

// Telnet接入模块操作接口（导出给cfg模块注册）
extern access_module_ops_t telnet_access_ops;

#ifdef __cplusplus
}
#endif

#endif // TELNET_ACCESS_H
