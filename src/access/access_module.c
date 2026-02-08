/**
 * @file   access_module.c
 * @brief  接入模块框架实现
 * @author jhb
 * @date   2026/02/07
 */
#include "access_module.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

// 已注册的接入模块列表
static GList *g_access_modules = NULL;

int access_module_register(access_module_ops_t *ops)
{
    if (!ops || !ops->name)
    {
        fprintf(stderr, "[access] Invalid module ops\n");
        return -1;
    }

    // 检查是否已注册
    for (GList *node = g_access_modules; node != NULL; node = node->next)
    {
        access_module_ops_t *existing = (access_module_ops_t *)node->data;
        if (strcmp(existing->name, ops->name) == 0)
        {
            fprintf(stderr, "[access] Module '%s' already registered\n", ops->name);
            return -1;
        }
    }

    g_access_modules = g_list_append(g_access_modules, ops);
    printf("[access] Registered module: %s\n", ops->name);
    return 0;
}

int access_module_start_all(void)
{
    int started = 0;

    for (GList *node = g_access_modules; node != NULL; node = node->next)
    {
        access_module_ops_t *ops = (access_module_ops_t *)node->data;
        
        printf("[access] Starting module: %s\n", ops->name);
        
        // 初始化
        if (ops->init && ops->init(NULL) < 0)
        {
            fprintf(stderr, "[access] Failed to initialize module: %s\n", ops->name);
            continue;
        }

        // 启动
        if (ops->start && ops->start() < 0)
        {
            fprintf(stderr, "[access] Failed to start module: %s\n", ops->name);
            if (ops->cleanup)
            {
                ops->cleanup();
            }
            continue;
        }

        started++;
        printf("[access] ✓ Module started: %s\n", ops->name);
    }

    return started;
}

void access_module_stop_all(void)
{
    for (GList *node = g_access_modules; node != NULL; node = node->next)
    {
        access_module_ops_t *ops = (access_module_ops_t *)node->data;
        
        printf("[access] Stopping module: %s\n", ops->name);
        
        if (ops->stop)
        {
            ops->stop();
        }
    }
}

void access_module_cleanup_all(void)
{
    for (GList *node = g_access_modules; node != NULL; node = node->next)
    {
        access_module_ops_t *ops = (access_module_ops_t *)node->data;
        
        if (ops->cleanup)
        {
            ops->cleanup();
        }
    }

    g_list_free(g_access_modules);
    g_access_modules = NULL;
    printf("[access] Cleanup complete\n");
}
