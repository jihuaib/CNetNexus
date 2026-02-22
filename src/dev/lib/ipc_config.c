/**
 * @file   ipc_config.c
 * @brief  IPC 配置文件解析实现
 * @author jhb
 * @date   2026/02/02
 */

#define LOG_TAG "ipc"

#include "ipc_config.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "errcode.h"
#include "log.h"
#include "path_utils.h"

int ipc_config_load(const char *path, ipc_config_t *config)
{
    if (!path || !config)
    {
        return ERRCODE_FAIL;
    }

    memset(config, 0, sizeof(ipc_config_t));

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        LOG_ERROR("无法打开配置文件: %s", path);
        return ERRCODE_FAIL;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        /* 跳过注释和空行 */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
        {
            continue;
        }

        if (config->num_modules >= 16)
        {
            LOG_ERROR("配置文件模块数超过上限");
            break;
        }

        ipc_module_addr_t *m = &config->modules[config->num_modules];
        char id_str[32] = {0};
        int parsed = sscanf(line, "%31s %31s %63s %hu", id_str, m->name, m->host, &m->port);
        if (parsed != 4)
        {
            continue;
        }

        /* 解析十六进制或十进制模块 ID */
        m->module_id = (uint32_t)strtoul(id_str, NULL, 0);

        config->num_modules++;
    }

    fclose(fp);
    LOG_INFO("加载配置: %d 个模块 (from %s)", config->num_modules, path);
    return ERRCODE_SUCCESS;
}

const ipc_module_addr_t *ipc_config_find(const ipc_config_t *config, uint32_t module_id)
{
    if (!config)
    {
        return NULL;
    }

    for (int i = 0; i < config->num_modules; i++)
    {
        if (config->modules[i].module_id == module_id)
        {
            return &config->modules[i];
        }
    }
    return NULL;
}

int ipc_resolve_config_path(const char *config_path, char *out_path, int out_size)
{
    if (!out_path || out_size <= 0)
    {
        return ERRCODE_FAIL;
    }

    /* 用户指定了路径 */
    if (config_path && config_path[0] != '\0')
    {
        snprintf(out_path, out_size, "%s", config_path);
        if (access(out_path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
    }

    /* 尝试环境变量 RESOURCES_DIR */
    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir)
    {
        snprintf(out_path, out_size, "%s/ipc/ipc.conf", resources_dir);
        if (access(out_path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
    }

    /* 尝试生产路径 */
    snprintf(out_path, out_size, "/opt/netnexus/resources/ipc/ipc.conf");
    if (access(out_path, R_OK) == 0)
    {
        return ERRCODE_SUCCESS;
    }

    /* 尝试开发路径（相对于可执行文件） */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        /* 优先使用 include/ipc.conf（便于统一修改） */
        snprintf(out_path, out_size, "%s/../../include/ipc.conf", exe_dir);
        if (access(out_path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }

        snprintf(out_path, out_size, "%s/../../src/ipc/resources/ipc.conf", exe_dir);
        if (access(out_path, R_OK) == 0)
        {
            return ERRCODE_SUCCESS;
        }
    }

    return ERRCODE_FAIL;
}
