/**
 * @file   access_module.h
 * @brief  接入模块通用接口定义
 * @author jhb
 * @date   2026/02/07
 */
#ifndef ACCESS_MODULE_H
#define ACCESS_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 接入模块操作接口
 */
typedef struct access_module_ops {
    const char *name;                           // 模块名称，如 "telnet", "ssh"
    
    /**
     * 初始化接入模块
     * @param config 配置数据（可选，模块自定义格式）
     * @return 0成功，-1失败
     */
    int (*init)(void *config);
    
    /**
     * 启动服务（监听端口等）
     * @return 0成功，-1失败
     */
    int (*start)(void);
    
    /**
     * 停止服务
     * @return 0成功，-1失败
     */
    int (*stop)(void);
    
    /**
     * 清理资源
     */
    void (*cleanup)(void);
} access_module_ops_t;

/**
 * 注册接入模块
 * @param ops 模块操作接口
 * @return 0成功，-1失败
 */
int access_module_register(access_module_ops_t *ops);

/**
 * 启动所有已注册的接入模块
 * @return 成功启动的模块数量
 */
int access_module_start_all(void);

/**
 * 停止所有接入模块
 */
void access_module_stop_all(void);

/**
 * 清理所有接入模块
 */
void access_module_cleanup_all(void);

#ifdef __cplusplus
}
#endif

#endif // ACCESS_MODULE_H
