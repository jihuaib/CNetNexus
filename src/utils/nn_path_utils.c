#include "nn_path_utils.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int nn_get_exe_dir(char *buf, size_t size)
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

int nn_resolve_xml_path(const char *module_name, char *buf, size_t size)
{
    const char *xml_dir = getenv("NN_XML_DIR");

    // Priority 1: Environment variable (for Docker/production)
    if (xml_dir != NULL)
    {
        // Check if it's Docker path
        if (strstr(xml_dir, "/opt/netnexus") != NULL)
        {
            snprintf(buf, size, "%s/%s_commands.xml", xml_dir, module_name);
        }
        else
        {
            snprintf(buf, size, "%s/commands.xml", xml_dir);
        }

        // Verify file exists
        struct stat st;
        if (stat(buf, &st) == 0)
        {
            return 0;
        }
    }

    // Priority 2: Relative to executable (for development builds)
    char exe_dir[PATH_MAX];
    if (nn_get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        // Try: <exe_dir>/../src/<module>/commands.xml
        snprintf(buf, size, "%s/../src/%s/commands.xml", exe_dir, module_name);
        struct stat st;
        if (stat(buf, &st) == 0)
        {
            return 0;
        }

        // Try: <exe_dir>/../../src/<module>/commands.xml (for build/bin/netnexus)
        snprintf(buf, size, "%s/../../src/%s/commands.xml", exe_dir, module_name);
        if (stat(buf, &st) == 0)
        {
            return 0;
        }
    }

    // Priority 3: Fallback to hardcoded relative path
    snprintf(buf, size, "../../src/%s/commands.xml", module_name);
    struct stat st;
    if (stat(buf, &st) == 0)
    {
        return 0;
    }

    // If all else fails, return the last attempted path
    fprintf(stderr, "[path] Warning: Could not find XML file for module '%s'\n", module_name);
    return -1;
}
