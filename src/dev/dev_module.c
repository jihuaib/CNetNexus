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
#include "dev_main.h"
#include "errcode.h"
#include "log.h"
#include "path_utils.h"

/* 前向声明 */
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data);
static pid_t dev_spawn_module(const char *exe_name, const char *module_name);

/* 全局模块注册表（GLib tree: id -> dev_module_t*） */
static GTree *g_module_registry = NULL;

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

        /* fork+exec 启动模块子进程 */
        pid_t child_pid = dev_spawn_module(conf.exe_name, conf.name);
        if (child_pid < 0)
        {
            LOG_ERROR("Failed to start module %s", conf.name);
            continue;
        }

        /* 创建模块并添加到注册表 */
        dev_module_t *module = dev_add_module_to_registry(conf.module_id, conf.name);
        if (!module)
        {
            LOG_ERROR("Module %s registration failed", conf.name);
            kill(child_pid, SIGKILL);
            continue;
        }

        module->child_pid = child_pid;
        module->port = conf.port;

        loaded++;
        LOG_INFO("Module %s started (pid=%d)", conf.name, child_pid);
    }

    closedir(dir);
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
static pid_t dev_spawn_module(const char *exe_name, const char *module_name)
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

    pid_t pid = fork();
    if (pid < 0)
    {
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
        if (dev_self->phase < DEV_PHASE_LOADED)
        {
            dev_self->phase = DEV_PHASE_LOADED;
        }
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

// ============================================================================
// Phase 1: 发送 MODULE_START RPC（无 payload，各模块名称已在 dev_ipc_init 中配置）
// ============================================================================

static gboolean phase1_start_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    int32_t *failed_count = (int32_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        module->phase = DEV_PHASE_IPC_READY;
        return FALSE;
    }

    if (module->phase < DEV_PHASE_LOADED)
    {
        return FALSE;
    }

    LOG_INFO("Phase 1: Sending MODULE_START -> %s", module->name);

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_START, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, NULL, 0, NULL);

    dev_ipc_message_t *resp = dev_ipc_query(g_dev_local->dev_ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 1: %s IPC connection established", module->name);
        module->phase = DEV_PHASE_IPC_READY;
        dev_ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 1: %s IPC connection timeout or failed", module->name);
        (*failed_count)++;
    }

    return FALSE;
}

// ============================================================================
// Phase 2: 发送 MODULE_CONNECT RPC
// ============================================================================

static gboolean phase2_connect_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    int32_t *failed_count = (int32_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        module->phase = DEV_PHASE_DB_RECOVERED;
        return FALSE;
    }

    if (module->phase < DEV_PHASE_IPC_READY)
    {
        return FALSE;
    }

    LOG_INFO("Phase 2: Sending MODULE_CONNECT -> %s", module->name);

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_CONNECT, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, NULL, 0, NULL);

    dev_ipc_message_t *resp = dev_ipc_query(g_dev_local->dev_ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 2: %s reserved phase complete", module->name);
        module->phase = DEV_PHASE_DB_RECOVERED;
        dev_ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 2: %s reserved phase timeout or failed", module->name);
        (*failed_count)++;
    }

    return FALSE;
}

// ============================================================================
// Phase 3: 发送 MODULE_READY RPC
// ============================================================================

static gboolean phase3_ready_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    int32_t *failed_count = (int32_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        module->phase = DEV_PHASE_READY;
        return FALSE;
    }

    if (module->phase < DEV_PHASE_DB_RECOVERED)
    {
        return FALSE;
    }

    LOG_INFO("Phase 3: Sending MODULE_READY -> %s", module->name);

    dev_ipc_message_t *req = dev_ipc_message_create(DEV_IPC_MSG_TYPE_DEV_MODULE_READY, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, NULL, 0, NULL);

    dev_ipc_message_t *resp = dev_ipc_query(g_dev_local->dev_ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 3: %s ready", module->name);
        module->phase = DEV_PHASE_READY;
        dev_ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 3: %s ready timeout or failed", module->name);
        (*failed_count)++;
    }

    return FALSE;
}

// ============================================================================
// 三阶段初始化主流程
// ============================================================================

int32_t dev_init_all_modules(void)
{
    /* 所有模块 constructor 执行完毕后，主线程的日志标签已被最后一个模块改写。
     * 在三阶段初始化期间统一恢复为 "dev"，使日志便于识别。 */
    log_set_tag("dev");

    int32_t failed_count = 0;

    LOG_INFO("=============================================");
    LOG_INFO("Starting three-phase module initialization");
    LOG_INFO("=============================================");

    if (!g_module_registry)
    {
        LOG_INFO("No registered modules");
        return ERRCODE_SUCCESS;
    }

    /* DEV connecting to all modules */
    LOG_INFO("DEV connecting to all modules");
    g_tree_foreach(g_module_registry, dev_connect_to_module_callback, g_dev_local->dev_ipc_ctx);

    /* 等待所有 IPC 连接完成握手（最多 5 秒） */
    LOG_INFO("Waiting for IPC connections to be ready...");
    int all_connected = 0;
    for (int retry = 0; retry < 50; retry++)
    {
        all_connected = 1;
        GList *wait_modules = NULL;
        g_tree_foreach(g_module_registry, collect_module_callback, &wait_modules);

        for (GList *l = wait_modules; l != NULL; l = l->next)
        {
            dev_module_t *m = (dev_module_t *)l->data;
            if (m->module_id != DEV_MODULE_ID_DEV && m->phase >= DEV_PHASE_LOADED)
            {
                if (!dev_ipc_is_connected(g_dev_local->dev_ipc_ctx, m->module_id))
                {
                    all_connected = 0;
                    if (retry % 10 == 9) /* 每 1 秒记录一次等待中的模块 */
                    {
                        LOG_WARN("Waiting for IPC connection: %s (id=0x%08X) not ready", m->name, m->module_id);
                    }
                }
            }
        }
        g_list_free(wait_modules);

        if (all_connected)
        {
            LOG_INFO("All IPC connections ready (took ~%d ms)", retry * 100);
            break;
        }
        usleep(100000); /* 100ms */
    }

    if (!all_connected)
    {
        LOG_WARN("Wait timeout (5 seconds), some module IPC connections not ready, continuing initialization");
        /* 打印每个模块的最终连接状态 */
        GList *mods = NULL;
        g_tree_foreach(g_module_registry, collect_module_callback, &mods);
        for (GList *l = mods; l != NULL; l = l->next)
        {
            dev_module_t *m = (dev_module_t *)l->data;
            if (m->module_id != DEV_MODULE_ID_DEV)
            {
                int connected = dev_ipc_is_connected(g_dev_local->dev_ipc_ctx, m->module_id);
                LOG_WARN("  Module %s (id=0x%08X): %s", m->name, m->module_id,
                         connected ? "connected" : "not connected");
            }
        }
        g_list_free(mods);
    }

    /* Phase 1: 发送 MODULE_START — 模块建立 IPC 连接 */
    LOG_INFO("=== Phase 1: MODULE_START (IPC setup) ===");
    g_tree_foreach(g_module_registry, phase1_start_callback, &failed_count);

    /* Phase 2: 发送 MODULE_CONNECT — 预留（DB 恢复） */
    LOG_INFO("=== Phase 2: MODULE_CONNECT (reserved) ===");
    g_tree_foreach(g_module_registry, phase2_connect_callback, &failed_count);
    LOG_INFO("Phase 2: All modules reserved phase complete");

    /* Phase 3: 发送 MODULE_READY — 预留（CFG 加载 XML） */
    LOG_INFO("=== Phase 3: MODULE_READY (reserved) ===");
    g_tree_foreach(g_module_registry, phase3_ready_callback, &failed_count);

    LOG_INFO("=============================================");
    LOG_INFO("Three-phase initialization complete (failed: %d)", failed_count);
    LOG_INFO("=============================================");

    return failed_count;
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

void cleanup_all_modules(void)
{
    LOG_INFO("==================== Cleaning up modules ====================");

    if (!g_module_registry)
    {
        LOG_INFO("No modules to clean up");
        return;
    }

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

        int exited = 0;
        for (int i = 0; i < 30; i++)
        {
            int status;
            pid_t r = waitpid(module->child_pid, &status, WNOHANG);
            if (r == module->child_pid)
            {
                LOG_INFO("Module %s (pid=%d) exited", module->name, module->child_pid);
                exited = 1;
                break;
            }
            usleep(100000); /* 100ms */
        }

        if (!exited)
        {
            LOG_WARN("Module %s (pid=%d) not responding, force killing", module->name, module->child_pid);
            kill(module->child_pid, SIGKILL);
            waitpid(module->child_pid, NULL, 0);
        }

        g_free(module);
    }

    /* Step 3: Destroying DEV */
    LOG_INFO("Destroying DEV");
    dev_module_t *dev_self = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (dev_self)
    {
        g_free(dev_self);
    }

    g_list_free(modules);

    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    LOG_INFO("Module cleanup complete");
}
