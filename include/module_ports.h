/**
 * @file   module_ports.h
 * @brief  各模块 IPC 监听端口配置
 *
 * 新增模块时在此文件添加端口常量，无需修改 IPC 库代码。
 * @author jhb
 * @date   2026/02/22
 */

#ifndef MODULE_PORTS_H
#define MODULE_PORTS_H

/** 本地回环地址 */
#define IPC_HOST_LOCAL "127.0.0.1"

/** DEV 模块 IPC 监听端口 */
#define MODULE_PORT_DEV 4001
/** DB 模块 IPC 监听端口 */
#define MODULE_PORT_DB 4002
/** CFG 模块 IPC 监听端口 */
#define MODULE_PORT_CFG 4003
/** IF 模块 IPC 监听端口 */
#define MODULE_PORT_IF 4004
/** BGP 模块 IPC 监听端口 */
#define MODULE_PORT_BGP 4005
/** ROUTE 模块 IPC 监听端口 */
#define MODULE_PORT_ROUTE 4006

#endif /* MODULE_PORTS_H */
