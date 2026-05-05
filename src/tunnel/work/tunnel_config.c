#include "tunnel_config.h"

#include <errno.h>
#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "errcode.h"
#include "log.h"
#include "path_utils.h"
#include "tunnel.h"

#define TUNNEL_CONFIG_FILE "tunnel.conf"

static char *tunnel_config_resolve_path(void)
{
    struct stat st;
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char *path = g_build_filename(work_dir, "resources", "tunnel", TUNNEL_CONFIG_FILE, NULL);
        if (stat(path, &st) == 0)
        {
            return path;
        }
        LOG_WARN("TUNNEL: config not found at %s", path);
        g_free(path);
    }

    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) != 0)
    {
        LOG_ERROR("TUNNEL: failed to resolve executable directory for config");
        return NULL;
    }

    char *path = g_build_filename(exe_dir, "..", "..", "src", "tunnel", "resources", TUNNEL_CONFIG_FILE, NULL);
    if (stat(path, &st) == 0)
    {
        return path;
    }

    LOG_ERROR("TUNNEL: config not found at %s", path);
    g_free(path);
    return NULL;
}

static int parse_u32_key(GKeyFile *kf, const char *path, const char *group, const char *key, uint32_t *out)
{
    if (!kf || !group || !key || !out)
    {
        return ERRCODE_FAIL;
    }

    GError *err = NULL;
    char *text = g_key_file_get_string(kf, group, key, &err);
    if (!text)
    {
        LOG_ERROR("TUNNEL: missing config key %s.%s in %s: %s", group, key, path ? path : "-",
                  err ? err->message : "-");
        g_clear_error(&err);
        return ERRCODE_FAIL;
    }

    char *end = NULL;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    while (end && g_ascii_isspace(*end))
    {
        end++;
    }
    if (errno != 0 || !end || *end != '\0' || value > UINT32_MAX)
    {
        LOG_ERROR("TUNNEL: invalid uint config %s.%s=%s in %s", group, key, text, path ? path : "-");
        g_free(text);
        return ERRCODE_FAIL;
    }

    *out = (uint32_t)value;
    g_free(text);
    return ERRCODE_SUCCESS;
}

int tunnel_config_load(tunnel_config_t *cfg)
{
    if (!cfg)
    {
        return ERRCODE_FAIL;
    }

    memset(cfg, 0, sizeof(*cfg));

    char *path = tunnel_config_resolve_path();
    if (!path)
    {
        return ERRCODE_FAIL;
    }

    GKeyFile *kf = g_key_file_new();
    GError *err = NULL;
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, &err);
    if (!ok)
    {
        LOG_ERROR("TUNNEL: failed to load config %s: %s", path, err ? err->message : "-");
        g_clear_error(&err);
        g_key_file_unref(kf);
        g_free(path);
        return ERRCODE_FAIL;
    }

    if (parse_u32_key(kf, path, "label", "dynamic_min", &cfg->label_dynamic_min) != ERRCODE_SUCCESS ||
        parse_u32_key(kf, path, "label", "dynamic_max", &cfg->label_dynamic_max) != ERRCODE_SUCCESS ||
        parse_u32_key(kf, path, "linux", "platform_labels", &cfg->linux_platform_labels) != ERRCODE_SUCCESS)
    {
        g_key_file_unref(kf);
        g_free(path);
        return ERRCODE_FAIL;
    }

    cfg->linux_mpls_input = g_key_file_get_boolean(kf, "linux", "mpls_input", &err);
    if (err)
    {
        LOG_ERROR("TUNNEL: invalid or missing config key linux.mpls_input in %s: %s", path, err->message);
        g_clear_error(&err);
        g_key_file_unref(kf);
        g_free(path);
        return ERRCODE_FAIL;
    }

    if (cfg->label_dynamic_min == 0 || cfg->label_dynamic_min > cfg->label_dynamic_max ||
        cfg->label_dynamic_max > TUNNEL_LABEL_VALUE_MAX)
    {
        LOG_ERROR("TUNNEL: invalid label range %u-%u in %s", cfg->label_dynamic_min, cfg->label_dynamic_max, path);
        g_key_file_unref(kf);
        g_free(path);
        return ERRCODE_FAIL;
    }

    if (cfg->linux_platform_labels == 0 || cfg->linux_platform_labels > TUNNEL_LABEL_VALUE_MAX ||
        cfg->label_dynamic_max >= cfg->linux_platform_labels)
    {
        LOG_ERROR("TUNNEL: linux.platform_labels=%u must be > label.dynamic_max=%u and <= %u in %s",
                  cfg->linux_platform_labels, cfg->label_dynamic_max, TUNNEL_LABEL_VALUE_MAX, path);
        g_key_file_unref(kf);
        g_free(path);
        return ERRCODE_FAIL;
    }

    LOG_INFO("TUNNEL: loaded config %s label_range=%u-%u platform_labels=%u mpls_input=%s", path,
             cfg->label_dynamic_min, cfg->label_dynamic_max, cfg->linux_platform_labels,
             cfg->linux_mpls_input ? "true" : "false");

    g_key_file_unref(kf);
    g_free(path);
    return ERRCODE_SUCCESS;
}
