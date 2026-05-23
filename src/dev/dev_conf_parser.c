/**
 * @file   dev_conf_parser.c
 * @brief  模块配置文件 (.conf) 解析器实现
 * @author jhb
 * @date   2026/02/14
 */
#include "dev_conf_parser.h"

#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "path_utils.h"

int dev_conf_parse(const char *path, dev_module_conf_t *conf)
{
    if (!path || !conf)
    {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp)
    {
        return -1;
    }

    memset(conf, 0, sizeof(*conf));

    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        /* 跳过注释和空行 */
        char *p = line;
        while (*p == ' ' || *p == '\t')
        {
            p++;
        }
        if (*p == '#' || *p == '\n' || *p == '\0')
        {
            continue;
        }

        /* 去除行末换行 */
        char *nl = strchr(p, '\n');
        if (nl)
        {
            *nl = '\0';
        }

        /* 解析 key=value */
        char *eq = strchr(p, '=');
        if (!eq)
        {
            continue;
        }

        *eq = '\0';
        char *key = p;
        char *value = eq + 1;

        /* 去除 key 末尾空格 */
        char *ke = eq - 1;
        while (ke >= key && (*ke == ' ' || *ke == '\t'))
        {
            *ke-- = '\0';
        }

        /* 去除 value 前导空格 */
        while (*value == ' ' || *value == '\t')
        {
            value++;
        }

        if (strcmp(key, "module-id") == 0)
        {
            conf->module_id = (uint32_t)atoi(value);
        }
        else if (strcmp(key, "name") == 0)
        {
            snprintf(conf->name, sizeof(conf->name), "%s", value);
        }
        else if (strcmp(key, "exe") == 0)
        {
            snprintf(conf->exe_name, sizeof(conf->exe_name), "%s", value);
        }
        else if (strcmp(key, "port") == 0)
        {
            conf->port = (uint16_t)atoi(value);
        }
        else if (strcmp(key, "on-demand") == 0)
        {
            conf->on_demand = (uint8_t)atoi(value);
        }
        else if (strcmp(key, "revive-table") == 0)
        {
            snprintf(conf->revive_table, sizeof(conf->revive_table), "%s", value);
        }
    }

    fclose(fp);
    return 0;
}

int dev_conf_resolve_and_parse(const char *module_name, dev_module_conf_t *conf)
{
    if (!module_name || !conf)
    {
        return -1;
    }

    struct stat st;
    char *path = NULL;
    int ret;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        path = g_build_filename(work_dir, "resources", module_name, "module.conf", NULL);
        if (stat(path, &st) == 0)
        {
            ret = dev_conf_parse(path, conf);
            g_free(path);
            return ret;
        }
        g_free(path);
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        path = g_build_filename(exe_dir, "..", "..", "src", module_name, "resources", "module.conf", NULL);
        if (stat(path, &st) == 0)
        {
            ret = dev_conf_parse(path, conf);
            g_free(path);
            return ret;
        }
        g_free(path);
    }

    return -1;
}
