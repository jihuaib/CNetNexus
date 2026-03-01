/**
 * @file   path_utils.c
 * @brief  路径工具函数实现
 * @author jhb
 * @date   2026/01/22
 */
#include "path_utils.h"

#include <glib.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

int get_exe_dir(char *buf, size_t size)
{
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (len == -1)
    {
        return -1;
    }

    exe_path[len] = '\0';

    // Find the last '/' to get directory
    char *last_slash = strrchr(exe_path, '/');
    if (last_slash == NULL)
    {
        return -1;
    }

    *last_slash = '\0';

    if (strlen(exe_path) >= size)
    {
        return -1;
    }

    strncpy(buf, exe_path, size);
    return 0;
}

int resolve_xml_path(const char *module_name, char *buf, size_t size)
{
    struct stat st;
    char *path = NULL;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir != NULL)
    {
        path = g_build_filename(work_dir, "resources", module_name, "commands.xml", NULL);
        if (stat(path, &st) == 0)
        {
            strlcpy(buf, path, size);
            g_free(path);
            return 0;
        }
        g_free(path);
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        path = g_build_filename(exe_dir, "..", "..", "src", module_name, "resources", "commands.xml", NULL);
        if (stat(path, &st) == 0)
        {
            strlcpy(buf, path, size);
            g_free(path);
            return 0;
        }
        g_free(path);
    }

    LOG_WARN("Could not find XML file for module '%s'", module_name);
    return -1;
}
