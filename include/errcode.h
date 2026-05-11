/**
 * @file   errcode.h
 * @brief  全局错误码定义
 * @author jhb
 * @date   2026/01/22
 */

#ifndef ERRCODE_H
#define ERRCODE_H

/** 操作成功 */
#define ERRCODE_SUCCESS 0
/** 操作失败 */
#define ERRCODE_FAIL (-1)
/** 数据库未打开 */
#define ERRCODE_DB_NOT_OPEN (-2)
/** 依赖暂未就绪：调用方应挂起重试（如等 VRF 设备出现） */
#define ERRCODE_DEP_MISSING (-3)

#endif // ERRCODE_H
