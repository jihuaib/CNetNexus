/**
 * @file   dev_module.c
 * @brief  Dev 模块注册和三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "dev_module.h"

#include <dirent.h>
#include <errno.h>
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "db.h"
#include "dev.h"
#include "dev_conf_parser.h"
#include "dev_db.h"
#include "dev_main.h"
#include "dev_subscribe.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

/* 模块 IPC 连接等待参数 */
#define DEV_INIT_IPC_WAIT_TIMEOUT_SEC 60U
#define DEV_INIT_IPC_WAIT_INTERVAL_USEC 100000U

/* 前向声明 */
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data);
static pid_t dev_spawn_module(const char *exe_name, const char *module_name, gboolean warm_restart);

/* 全局模块注册表（GLib tree: id -> dev_module_t*） */
static GTree *g_module_registry = NULL;

/* 批量清理标志（SIGCHLD handler 据此抑制"crashed/respawn"误报） */
static volatile gint g_cleanup_in_progress = 0;

void dev_module_set_cleanup_in_progress(int flag)
{
    g_atomic_int_set(&g_cleanup_in_progress, flag ? 1 : 0);
}

int dev_module_is_cleanup_in_progress(void)
{
    return g_atomic_int_get(&g_cleanup_in_progress);
}

/* GTree 比较函数 */
static gint module_id_compare(gconstpointer a, gconstpointer b)
{
    uint32_t id_a = GPOINTER_TO_UINT(a);
    uint32_t id_b = GPOINTER_TO_UINT(b);

    if (id_a < id_b)
    {
        return -1;
    }
    if (id_a > id_b)
    {
        return 1;
    }
    return 0;
}

/* 确保注册表已初始化 */
static void ensure_registry_initialized(void)
{
    if (!g_module_registry)
    {
        g_module_registry = g_tree_new(module_id_compare);
    }
}

static gint module_conf_id_compare(gconstpointer a, gconstpointer b)
{
    const dev_module_conf_t *conf_a = (const dev_module_conf_t *)a;
    const dev_module_conf_t *conf_b = (const dev_module_conf_t *)b;

    if (conf_a->module_id < conf_b->module_id)
    {
        return -1;
    }
    if (conf_a->module_id > conf_b->module_id)
    {
        return 1;
    }
    return 0;
}

// ============================================================================
// 辅助函数
// ============================================================================

int dev_get_module_name_inner(uint32_t module_id, char *module_name)
{
    module_name[0] = '\0';

    if (!g_module_registry)
    {
        return ERRCODE_FAIL;
    }

    dev_module_t *module = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(module_id));
    if (module)
    {
        strlcpy(module_name, module->name, DEV_MODULE_NAME_MAX_LEN);
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

/* 按名称查找模块 ID 的遍历上下文 */
typedef struct
{
    const char *name;
    uint32_t module_id;
    int found;
} find_by_name_ctx_t;

static gboolean find_module_by_name_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    find_by_name_ctx_t *ctx = (find_by_name_ctx_t *)data;

    if (strcmp(module->name, ctx->name) == 0)
    {
        ctx->module_id = module->module_id;
        ctx->found = 1;
        return TRUE; /* 停止遍历 */
    }
    return FALSE;
}

int dev_get_module_id_by_name(const char *name, uint32_t *module_id)
{
    if (!g_module_registry || !name || !module_id)
    {
        return ERRCODE_FAIL;
    }

    find_by_name_ctx_t ctx = {.name = name, .module_id = 0, .found = 0};
    g_tree_foreach(g_module_registry, find_module_by_name_cb, &ctx);

    if (ctx.found)
    {
        *module_id = ctx.module_id;
        return ERRCODE_SUCCESS;
    }

    return ERRCODE_FAIL;
}

dev_module_t *dev_module_find(uint32_t module_id)
{
    if (!g_module_registry)
    {
        return NULL;
    }
    return (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(module_id));
}

/* 按 pid 查找模块的遍历上下文 */
typedef struct
{
    pid_t pid;
    dev_module_t *found;
} find_by_pid_ctx_t;

static gboolean find_module_by_pid_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    find_by_pid_ctx_t *ctx = (find_by_pid_ctx_t *)data;

    /* PRE_EXIT 路径会把 child_pid 清零、原 pid 转存到 pre_cleaned_pid，
     * 这里两个字段都要匹配，否则 SIGCHLD reap 时找不到模块。 */
    if (module->child_pid == ctx->pid || module->pre_cleaned_pid == ctx->pid)
    {
        ctx->found = module;
        return TRUE; /* 停止遍历 */
    }
    return FALSE;
}

dev_module_t *dev_module_find_by_pid(pid_t pid)
{
    if (!g_module_registry || pid <= 0)
    {
        return NULL;
    }
    find_by_pid_ctx_t ctx = {.pid = pid, .found = NULL};
    g_tree_foreach(g_module_registry, find_module_by_pid_cb, &ctx);
    return ctx.found;
}

int dev_module_spawn_on_demand(dev_module_t *module)
{
    if (!module)
    {
        return ERRCODE_FAIL;
    }
    if (!module->on_demand)
    {
        LOG_WARN("Module %s is not on-demand, refuse to spawn via on-demand path", module->name);
        return ERRCODE_FAIL;
    }
    return dev_module_respawn(module);
}

int dev_module_respawn(dev_module_t *module)
{
    if (!module)
    {
        return ERRCODE_FAIL;
    }
    if (module->child_pid > 0)
    {
        /* 已有进程在跑（可能仍在初始化），幂等返回 */
        return ERRCODE_SUCCESS;
    }
    if (module->exe_name[0] == '\0')
    {
        LOG_ERROR("Module %s has no exe_name, cannot spawn", module->name);
        return ERRCODE_FAIL;
    }

    /* respawn 路径（process start/reboot、on-demand 拉起、revive）都发生在系统已运行时，
     * 属"热重启"：DB 子进程据此保留磁盘上的 running.db，不当残留清掉。 */
    pid_t pid = dev_spawn_module(module->exe_name, module->name, TRUE);
    if (pid < 0)
    {
        return ERRCODE_FAIL;
    }
    module->child_pid = pid;
    module->phase = DEV_PHASE_LOADED;
    LOG_INFO("Module %s spawned (pid=%d, on_demand=%u)", module->name, pid, module->on_demand);
    return ERRCODE_SUCCESS;
}

void dev_module_foreach(GTraverseFunc func, gpointer user_data)
{
    if (g_module_registry)
    {
        g_tree_foreach(g_module_registry, func, user_data);
    }
}

dev_module_t *dev_add_module_to_registry(uint32_t module_id, const char *name)
{
    ensure_registry_initialized();

    dev_module_t *module = g_malloc0(sizeof(dev_module_t));
    module->module_id = module_id;
    strlcpy(module->name, name, DEV_MODULE_NAME_MAX_LEN);
    module->phase = DEV_PHASE_LOADED;

    g_tree_insert(g_module_registry, GUINT_TO_POINTER(module_id), module);
    LOG_INFO("Module %s (id=%u) added to registry", name, module_id);

    return module;
}

// ============================================================================
// 扫描目录并动态加载所有模块
// ============================================================================

/**
 * @brief 尝试在 base_dir 下扫描子目录中的 module.conf
 * @return 成功加载的模块数
 */
static int dev_scan_dir_for_modules(const char *base_dir)
{
    DIR *dir = opendir(base_dir);
    if (!dir)
    {
        return 0;
    }

    int loaded = 0;
    struct dirent *entry;
    GArray *module_confs = g_array_new(FALSE, FALSE, sizeof(dev_module_conf_t));
    if (!module_confs)
    {
        closedir(dir);
        return 0;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        /* 检查子目录下是否有 module.conf
         * 兼容两种布局：
         *   dev  布局: {base_dir}/{module}/resources/module.conf  (源码树)
         *   prod 布局: {base_dir}/{module}/module.conf            (部署包) */
        struct stat st;
        char *conf_path = g_build_filename(base_dir, entry->d_name, "resources", "module.conf", NULL);
        if (stat(conf_path, &st) != 0)
        {
            g_free(conf_path);
            conf_path = g_build_filename(base_dir, entry->d_name, "module.conf", NULL);
            if (stat(conf_path, &st) != 0)
            {
                g_free(conf_path);
                continue;
            }
        }

        /* 解析 module.conf */
        dev_module_conf_t conf;
        if (dev_conf_parse(conf_path, &conf) != 0)
        {
            LOG_ERROR("Failed to parse %s", conf_path);
            g_free(conf_path);
            continue;
        }
        g_free(conf_path);
        conf_path = NULL;

        /* 跳过 DEV 模块（由 dev_init_self 单独处理） */
        if (conf.module_id == DEV_MODULE_ID_DEV)
        {
            continue;
        }

        /* 必须有 exe */
        if (conf.exe_name[0] == '\0')
        {
            LOG_WARN("Module %s missing exe field, skipping", conf.name);
            continue;
        }

        LOG_INFO("Found module: %s (id=%u, exe=%s)", conf.name, conf.module_id, conf.exe_name);
        g_array_append_val(module_confs, conf);
    }

    closedir(dir);

    g_array_sort(module_confs, module_conf_id_compare);

    for (guint i = 0; i < module_confs->len; i++)
    {
        dev_module_conf_t *conf = &g_array_index(module_confs, dev_module_conf_t, i);

        /* 创建模块条目（无论是否按需都先入注册表） */
        dev_module_t *module = dev_add_module_to_registry(conf->module_id, conf->name);
        if (!module)
        {
            LOG_ERROR("Module %s registration failed", conf->name);
            continue;
        }
        module->port = conf->port;
        module->on_demand = conf->on_demand;
        strlcpy(module->exe_name, conf->exe_name, sizeof(module->exe_name));
        strlcpy(module->revive_table, conf->revive_table, sizeof(module->revive_table));

        /* 按需模块：留到首个订阅请求时再 fork
         * 注意 phase 保持默认 DEV_PHASE_LOADED（由 dev_add_module_to_registry 设置）
         * 改为 DEV_PHASE_REGISTERED 表示"未运行"，三阶段回调和 IPC 等待都会跳过它 */
        if (conf->on_demand)
        {
            module->phase = DEV_PHASE_REGISTERED;
            LOG_INFO("Module %s registered as on-demand (deferred spawn)", conf->name);
            loaded++;
            continue;
        }

        /* 常驻模块：fork+exec 启动子进程（整机冷启动路径，warm_restart=FALSE） */
        pid_t child_pid = dev_spawn_module(conf->exe_name, conf->name, FALSE);
        if (child_pid < 0)
        {
            LOG_ERROR("Failed to start module %s", conf->name);
            continue;
        }

        module->child_pid = child_pid;
        loaded++;
        LOG_INFO("Module %s started (pid=%d)", conf->name, child_pid);
    }

    g_array_free(module_confs, TRUE);
    return loaded;
}

/**
 * @brief 关闭除 stdin/stdout/stderr 之外的所有文件描述符
 *
 * 在 fork 后的子进程中调用，防止子进程继承 DEV 的 IPC socket 等资源。
 */
static void close_inherited_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    if (!dir)
    {
        /* 回退：按上限遍历 */
        int max_fd = (int)sysconf(_SC_OPEN_MAX);
        for (int i = 3; i < max_fd; i++)
        {
            close(i);
        }
        return;
    }

    int dir_fd = dirfd(dir);
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        if (ent->d_name[0] == '.')
        {
            continue;
        }
        int fd = atoi(ent->d_name);
        if (fd > 2 && fd != dir_fd)
        {
            close(fd);
        }
    }
    closedir(dir);
}

/**
 * @brief 解析可执行文件路径并 fork+exec 启动模块子进程
 *
 * 查找顺序：
 *   1. 与 netnexus 同目录（开发环境 build/bin/）
 *   2. NN_WORK_DIR/bin/（生产环境）
 */
static pid_t dev_spawn_module(const char *exe_name, const char *module_name, gboolean warm_restart)
{
    char exe_path[PATH_MAX] = {0};

    /* 优先：与 DEV 可执行文件同目录 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char *p = g_build_filename(exe_dir, exe_name, NULL);
        if (access(p, X_OK) == 0)
        {
            strlcpy(exe_path, p, sizeof(exe_path));
        }
        g_free(p);
    }

    /* 其次：NN_WORK_DIR/bin/ */
    if (exe_path[0] == '\0')
    {
        const char *work_dir = getenv("NN_WORK_DIR");
        if (work_dir)
        {
            char *p = g_build_filename(work_dir, "bin", exe_name, NULL);
            if (access(p, X_OK) == 0)
            {
                strlcpy(exe_path, p, sizeof(exe_path));
            }
            g_free(p);
        }
    }

    if (exe_path[0] == '\0')
    {
        LOG_ERROR("Module executable not found: %s", exe_name);
        return -1;
    }

    /* 热重启标记：在 fork 前 setenv，子进程经 fork 继承该环境变量（execv 后仍保留），
     * 父进程随即 unsetenv，避免影响后续 spawn 或 DEV 自身。DB 子进程据此在
     * db_config_boot_prepare 中保留磁盘上的 running.db（见 db_config.c）。 */
    if (warm_restart)
    {
        setenv("NN_WARM_RESTART", "1", 1);
    }

    pid_t pid = fork();
    if (pid < 0)
    {
        if (warm_restart)
        {
            unsetenv("NN_WARM_RESTART");
        }
        LOG_ERROR("fork failed (%s): %s", module_name, strerror(errno));
        return -1;
    }

    if (pid == 0)
    {
        /* 子进程：关闭继承自 DEV 的所有 fd，防止端口/资源冲突 */
        close_inherited_fds();

        char *const args[] = {(char *)exe_name, NULL};
        execv(exe_path, args);

        /* execv 失败 */
        fprintf(stderr, "[dev] execv(%s) failed: %s\n", exe_path, strerror(errno));
        _exit(127);
    }

    /* 父进程：清掉临时环境变量（子进程已在 fork 时继承其副本） */
    if (warm_restart)
    {
        unsetenv("NN_WARM_RESTART");
    }

    return pid;
}

int32_t dev_scan_and_load_modules(void)
{
    ensure_registry_initialized();

    /* 首次启动需要创建 DEV IPC context；软件重启路径复用现有 context。 */
    if (!g_dev_local || !g_dev_local->dev_ipc_ctx)
    {
        /* DEV 自身初始化（创建 DEV 的 IPC context + 注册到 GTree）
         * dev_ipc_init() 内部会把调用线程标签设为 "dev"，在扫描模块之前日志已标记为 "dev" */
        if (dev_init_self() != ERRCODE_SUCCESS)
        {
            LOG_ERROR("Fatal: DEV module self-init failed");
            return ERRCODE_FAIL;
        }
    }
    else
    {
        dev_module_t *dev_self = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
        if (!dev_self)
        {
            dev_self = dev_add_module_to_registry(DEV_MODULE_ID_DEV, "dev");
            if (!dev_self)
            {
                LOG_ERROR("Fatal: failed to register DEV module in registry");
                return ERRCODE_FAIL;
            }
        }
        dev_self->port = DEV_MODULE_PORT_DEV;
        /* 软重启路径：把 DEV 的 phase 回退到 LOADED，结束后由 dev_init_all_modules 再次置 READY */
        dev_self->phase = DEV_PHASE_LOADED;
    }

    LOG_INFO("Begin scanning and loading modules=============================================");

    int total_loaded = 0;

    /* 优先级 1: 环境变量 NN_WORK_DIR（生产环境） */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        char *resources_dir = g_build_filename(work_dir, "resources", NULL);
        total_loaded = dev_scan_dir_for_modules(resources_dir);
        g_free(resources_dir);
        if (total_loaded > 0)
        {
            LOG_INFO("Loaded %d modules from NN_WORK_DIR", total_loaded);
            return ERRCODE_SUCCESS;
        }
    }

    /* 优先级 2: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char *dev_path = g_build_filename(exe_dir, "..", "..", "src", NULL);
        total_loaded = dev_scan_dir_for_modules(dev_path);
        g_free(dev_path);
        if (total_loaded > 0)
        {
            LOG_INFO("Loaded %d modules from dev path", total_loaded);
            return ERRCODE_SUCCESS;
        }
    }

    if (total_loaded == 0)
    {
        LOG_ERROR("No modules found");
        return ERRCODE_FAIL;
    }

    LOG_INFO("End scanning and loading modules=============================================");

    return ERRCODE_SUCCESS;
}

int32_t dev_reboot_software(void)
{
    LOG_WARN("Software reboot requested: restarting all modules");

    cleanup_all_modules();

    /* 清理 DEV IPC 连接状态，避免复用旧连接对象导致重连受阻。 */
    if (g_dev_local && g_dev_local->dev_ipc_ctx)
    {
        dev_ipc_clear_connections(g_dev_local->dev_ipc_ctx);
    }

    if (dev_scan_and_load_modules() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Software reboot failed: scan/load modules failed");
        return ERRCODE_FAIL;
    }

    if (dev_init_all_modules() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Software reboot failed: module initialization failed");
        return ERRCODE_FAIL;
    }

    LOG_INFO("Software reboot complete");
    return ERRCODE_SUCCESS;
}

/* DEV connecting to all modules */
static gboolean dev_connect_to_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    dev_ipc_context_t *dev_ctx = (dev_ipc_context_t *)data;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        return FALSE;
    }

    if (module->phase >= DEV_PHASE_LOADED)
    {
        LOG_INFO("Connecting to module: %s (port=%u)", module->name, module->port);
        dev_ipc_connect(dev_ctx, module->module_id, DEV_IPC_HOST_LOCAL, module->port);
    }

    return FALSE;
}

typedef struct
{
    dev_ipc_context_t *ctx;
    GString *pending_names;
    uint32_t pending_count;
} dev_ipc_wait_ctx_t;

typedef struct
{
    uint8_t required_phase;
    GString *pending_names;
    uint32_t pending_count;
} dev_phase_wait_ctx_t;

static void append_pending_name(GString *out, const char *name)
{
    if (!out || !name)
    {
        return;
    }
    if (out->len > 0)
    {
        g_string_append(out, ", ");
    }
    g_string_append(out, name);
}

/* 收集“已加载但 IPC 未连接”的模块（不含 DEV） */
static gboolean collect_ipc_pending_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    dev_ipc_wait_ctx_t *ctx = (dev_ipc_wait_ctx_t *)data;

    if (!module || !ctx || module->module_id == DEV_MODULE_ID_DEV || module->phase < DEV_PHASE_LOADED)
    {
        return FALSE;
    }

    if (!dev_ipc_is_connected(ctx->ctx, module->module_id))
    {
        ctx->pending_count++;
        append_pending_name(ctx->pending_names, module->name);
    }

    return FALSE;
}

/* 收集“阶段低于 required_phase”的模块（不含 DEV）。
 * 已 fork（phase >= LOADED）的模块都参与门控，无论 on-demand 与否；
 * 这样 revive 之后 on-demand 模块也会被等到 READY。
 * 未 fork（REGISTERED）的 on-demand 模块由于 phase < LOADED 自然被跳过。 */
static gboolean collect_phase_pending_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    dev_phase_wait_ctx_t *ctx = (dev_phase_wait_ctx_t *)data;

    if (!module || !ctx || module->module_id == DEV_MODULE_ID_DEV || module->phase < DEV_PHASE_LOADED)
    {
        return FALSE;
    }

    if (module->phase < ctx->required_phase)
    {
        ctx->pending_count++;
        append_pending_name(ctx->pending_names, module->name);
    }

    return FALSE;
}

/* 启动阶段：等待所有已加载模块 IPC 连接 ready，未完成则失败返回 */
static int dev_wait_all_ipc_ready(dev_ipc_context_t *dev_ctx, uint32_t timeout_sec)
{
    gint64 deadline_usec = g_get_monotonic_time() + (gint64)timeout_sec * G_USEC_PER_SEC;
    uint32_t round = 0;

    while (g_get_monotonic_time() < deadline_usec)
    {
        GString *pending = g_string_new("");
        if (!pending)
        {
            return ERRCODE_FAIL;
        }

        dev_ipc_wait_ctx_t ctx = {
            .ctx = dev_ctx,
            .pending_names = pending,
            .pending_count = 0,
        };
        g_tree_foreach(g_module_registry, collect_ipc_pending_cb, &ctx);

        if (ctx.pending_count == 0)
        {
            LOG_INFO("All IPC connections ready");
            g_string_free(pending, TRUE);
            return ERRCODE_SUCCESS;
        }

        if (round % 10 == 0)
        {
            LOG_WARN("Waiting for IPC connection: %s", pending->str);
        }

        g_string_free(pending, TRUE);
        usleep(DEV_INIT_IPC_WAIT_INTERVAL_USEC);
        round++;
    }

    GString *pending = g_string_new("");
    if (!pending)
    {
        return ERRCODE_FAIL;
    }
    dev_ipc_wait_ctx_t ctx = {
        .ctx = dev_ctx,
        .pending_names = pending,
        .pending_count = 0,
    };
    g_tree_foreach(g_module_registry, collect_ipc_pending_cb, &ctx);

    LOG_ERROR("IPC wait timeout (%u sec), still not connected: %s", timeout_sec,
              ctx.pending_count ? pending->str : "-");
    g_string_free(pending, TRUE);
    return (ctx.pending_count > 0) ? (int)ctx.pending_count : ERRCODE_FAIL;
}

/* 阶段门控：要求所有模块 phase >= required_phase */
static int dev_require_all_modules_phase(uint8_t required_phase, const char *phase_name)
{
    GString *pending = g_string_new("");
    if (!pending)
    {
        return ERRCODE_FAIL;
    }

    dev_phase_wait_ctx_t ctx = {
        .required_phase = required_phase,
        .pending_names = pending,
        .pending_count = 0,
    };
    g_tree_foreach(g_module_registry, collect_phase_pending_cb, &ctx);

    if (ctx.pending_count > 0)
    {
        LOG_ERROR("%s incomplete, pending modules: %s", phase_name, pending->str);
        g_string_free(pending, TRUE);
        return (int)ctx.pending_count;
    }

    g_string_free(pending, TRUE);
    return ERRCODE_SUCCESS;
}

// ============================================================================
// 按需模块自动恢复（DB 已就绪后调用）
// ============================================================================

/**
 * 对每个 on_demand 且声明了 revive_table 的模块，检查任一标识表中是否存在
 * 配置；存在即 fork 该模块（使其完成 db_restore 并自动 listen/服务）。
 * revive-table 支持逗号分隔的表名，用于兼容旧版本可能只留下子表配置、
 * 尚未建立 canonical singleton marker 的 running DB。
 *
 * Why: 用户配置过的模块在 NetNexus 重启后应该自动恢复业务，无需用户重新触发 CLI。
 */
static gboolean revive_on_demand_cb(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    dev_module_t *module = (dev_module_t *)value;

    if (!module || !module->on_demand || module->revive_table[0] == '\0')
    {
        return FALSE;
    }
    if (module->phase >= DEV_PHASE_LOADED || module->child_pid > 0)
    {
        return FALSE; /* 已经在跑（可能刚被另一个 CLI 命令触发） */
    }

    /*
     * 查任一表是否非空。模块尚未创建过的表会查询失败，按“不存在”跳过；
     * 只要一个表可查且非空，就必须拉起模块完成恢复/历史数据归一化。
     */
    gboolean exists = FALSE;
    char matched_table[64] = {0};
    gchar **tables = g_strsplit(module->revive_table, ",", -1);
    for (guint i = 0; tables && tables[i]; ++i)
    {
        char *table = g_strstrip(tables[i]);
        if (table[0] == '\0')
        {
            continue;
        }

        gboolean table_has_rows = FALSE;
        int rc = db_rpc_exists(g_dev_local->dev_ipc_ctx, table, NULL, &table_has_rows);
        if (rc != ERRCODE_SUCCESS)
        {
            LOG_DEBUG("Revive check: %s/%s not present in DB, skip", module->name, table);
            continue;
        }
        if (table_has_rows)
        {
            exists = TRUE;
            g_strlcpy(matched_table, table, sizeof(matched_table));
            break;
        }
    }
    g_strfreev(tables);

    if (!exists)
    {
        LOG_INFO("Revive check: %s has no rows in [%s], skip", module->name, module->revive_table);
        return FALSE;
    }

    LOG_INFO("Revive: %s has config in %s, auto-spawning", module->name, matched_table);
    if (dev_module_spawn_on_demand(module) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Revive: failed to spawn %s", module->name);
        return FALSE;
    }
    /* 主动建联，与 SUBSCRIBE 路径一致；IPC 库异步握手 + 自动重试 */
    dev_ipc_connect(g_dev_local->dev_ipc_ctx, module->module_id, DEV_IPC_HOST_LOCAL, module->port);
    return FALSE;
}

static void dev_revive_on_demand_modules(void)
{
    if (!g_module_registry || !g_dev_local || !g_dev_local->dev_ipc_ctx)
    {
        return;
    }
    LOG_INFO("=== Reviving on-demand modules with persisted config ===");
    g_tree_foreach(g_module_registry, revive_on_demand_cb, NULL);
}

// ============================================================================
// 初始化主流程（基础模块在自身 init 中调 notify_ready，DEV 等所有就绪）
// ============================================================================

int32_t dev_init_all_modules(void)
{
    /* 所有模块 constructor 执行完毕后，主线程的日志标签已被最后一个模块改写。
     * 在初始化期间统一恢复为 "dev"，使日志便于识别。 */
    log_set_tag("dev");

    LOG_INFO("=============================================");
    LOG_INFO("Starting module initialization (notify_ready model)");
    LOG_INFO("=============================================");

    if (!g_module_registry)
    {
        LOG_INFO("No registered modules");
        return ERRCODE_SUCCESS;
    }

    /* DEV 主动连接所有 LOADED（基础）模块；on-demand 模块跳过 */
    LOG_INFO("DEV connecting to all loaded modules");
    g_tree_foreach(g_module_registry, dev_connect_to_module_callback, g_dev_local->dev_ipc_ctx);

    /* 等待 IPC 链路全部就绪 */
    LOG_INFO("Waiting for IPC connections to be ready...");
    if (dev_wait_all_ipc_ready(g_dev_local->dev_ipc_ctx, DEV_INIT_IPC_WAIT_TIMEOUT_SEC) != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Abort initialization: not all module IPC connections are ready");
        return ERRCODE_FAIL;
    }

    /* DEV 自身的 DB 初始化与恢复（DB 已通过 notify_ready 报告就绪也好，连接已建立即可发 RPC） */
    if (dev_db_init() == 0)
    {
        if (dev_db_restore() == 0)
        {
            /* 恢复后立即广播给所有已连接模块，使日志级别全局一致 */
            dev_broadcast_log_level((uint32_t)log_get_level());
            syslog_report_remote_config_t syslog_cfg;
            syslog_report_get_remote(&syslog_cfg);
            dev_broadcast_syslog_remote(&syslog_cfg);
        }
    }

    /* 等待所有基础模块 notify_ready（即 phase == READY），超时仍继续 */
    LOG_INFO("Waiting for all basic modules to notify_ready...");
    gint64 deadline = g_get_monotonic_time() + (gint64)DEV_INIT_IPC_WAIT_TIMEOUT_SEC * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline)
    {
        if (dev_require_all_modules_phase(DEV_PHASE_READY, "ready") == ERRCODE_SUCCESS)
        {
            break;
        }
        usleep(DEV_INIT_IPC_WAIT_INTERVAL_USEC);
    }
    if (dev_require_all_modules_phase(DEV_PHASE_READY, "ready") != ERRCODE_SUCCESS)
    {
        LOG_WARN("Some basic modules did not notify_ready within timeout (continuing)");
    }

    LOG_INFO("=============================================");
    LOG_INFO("Basic modules initialization complete");
    LOG_INFO("=============================================");

    /* 按需模块自动恢复：检查每个 on-demand 模块的 revive_table，非空即 fork。
     * 在基础模块就绪后执行，确保 DB 可用、其它模块表已存在。 */
    dev_revive_on_demand_modules();

    /* revive 之后再次等所有已 fork 的模块（含 on-demand）达到 READY。
     * collect_phase_pending_cb 不再跳过 on-demand，自然把它们纳入门控。
     * 没有被 revive 的 on-demand 模块仍处于 REGISTERED（< LOADED），被天然跳过。 */
    LOG_INFO("Waiting for revived on-demand modules to notify_ready...");
    gint64 deadline2 = g_get_monotonic_time() + (gint64)DEV_INIT_IPC_WAIT_TIMEOUT_SEC * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline2)
    {
        if (dev_require_all_modules_phase(DEV_PHASE_READY, "ready") == ERRCODE_SUCCESS)
        {
            break;
        }
        usleep(DEV_INIT_IPC_WAIT_INTERVAL_USEC);
    }
    if (dev_require_all_modules_phase(DEV_PHASE_READY, "ready") != ERRCODE_SUCCESS)
    {
        LOG_WARN("Some on-demand modules did not notify_ready within timeout (continuing)");
    }

    /* DEV 自身的 READY：含义是 supervisor 已完成所有启动工作（基础模块 ready + DB 恢复 + on-demand revive）。
     * 业务模块此时如果 subscribe(DEV) 会立即拿到 READY 状态。 */
    dev_module_t *dev_self = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (dev_self)
    {
        dev_self->phase = DEV_PHASE_READY;
        dev_self->epoch++;
        dev_subscribe_broadcast_event(dev_self, DEV_MODULE_EVENT_READY);
    }

    LOG_INFO("=============================================");
    LOG_INFO("All modules ready (basic + revived on-demand + dev)");
    LOG_INFO("=============================================");

    return ERRCODE_SUCCESS;
}

// ============================================================================
// 清理流程（逆序）
// ============================================================================

/* 收集到列表以便逆序遍历 */
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    GList **list = (GList **)data;
    *list = g_list_prepend(*list, value);
    return FALSE;
}

static void dev_module_free(dev_module_t *module)
{
    if (!module)
    {
        return;
    }

    g_list_free(module->subscribers);
    module->subscribers = NULL;
    g_free(module);
}

void cleanup_all_modules(void)
{
    LOG_INFO("==================== Cleaning up modules ====================");

    if (!g_module_registry)
    {
        LOG_INFO("No modules to clean up");
        return;
    }

    /* 进入清理：抑制 SIGCHLD handler 的"crash"判定和 pending_restart 重启 */
    dev_module_set_cleanup_in_progress(1);

    /* 收集模块，g_list_prepend 得到逆序（高 ID 先） */
    GList *modules = NULL;
    g_tree_foreach(g_module_registry, collect_module_callback, &modules);

    /* Step 1: 向所有子进程发 SIGTERM，等待优雅退出 */
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_t *module = (dev_module_t *)l->data;

        if (module->module_id == DEV_MODULE_ID_DEV || module->child_pid <= 0)
        {
            continue;
        }

        LOG_INFO("Terminating module process: %s (pid=%d)", module->name, module->child_pid);
        kill(module->child_pid, SIGTERM);
    }

    /* Step 2: 等待子进程退出（最多 3 秒），超时则 SIGKILL */
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_t *module = (dev_module_t *)l->data;

        if (module->module_id == DEV_MODULE_ID_DEV || module->child_pid <= 0)
        {
            continue;
        }

        const pid_t child_pid = module->child_pid;
        int exited = 0;
        int wait_failed = 0;
        for (int i = 0; i < 30; i++)
        {
            int status;
            pid_t r;
            do
            {
                r = waitpid(child_pid, &status, WNOHANG);
            } while (r < 0 && errno == EINTR);

            if (r == child_pid)
            {
                LOG_INFO("Module %s (pid=%d) exited", module->name, child_pid);
                exited = 1;
                break;
            }
            if (r < 0)
            {
                if (errno == ECHILD)
                {
                    /* The main SIGCHLD loop may already have reaped this pid. */
                    LOG_INFO("Module %s (pid=%d) was already reaped", module->name, child_pid);
                    exited = 1;
                }
                else
                {
                    LOG_WARN("waitpid for module %s (pid=%d) failed: %s", module->name, child_pid, strerror(errno));
                    wait_failed = 1;
                }
                break;
            }
            usleep(100000); /* 100ms */
        }

        if (!exited && !wait_failed)
        {
            LOG_WARN("Module %s (pid=%d) not responding, force killing", module->name, child_pid);
            kill(child_pid, SIGKILL);

            pid_t r;
            do
            {
                r = waitpid(child_pid, NULL, 0);
            } while (r < 0 && errno == EINTR);

            if (r < 0 && errno != ECHILD)
            {
                LOG_WARN("waitpid after force killing module %s (pid=%d) failed: %s", module->name, child_pid,
                         strerror(errno));
            }
        }
    }

    /*
     * Step 3: release every registry value exactly once.
     *
     * child_pid only describes process state; it must not decide ownership of
     * the registry object.  In particular, on-demand/down modules have
     * child_pid == 0 and still own both dev_module_t and their subscriber
     * GList.  The tree has no value destroy callback, so free all collected
     * values before destroying its nodes.
     */
    LOG_INFO("Destroying module registry");
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_free((dev_module_t *)l->data);
    }

    g_list_free(modules);

    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    LOG_INFO("Module cleanup complete");
    dev_module_set_cleanup_in_progress(0);
}
