/**
 * @file   dev_module.c
 * @brief  Dev 模块注册和三阶段初始化
 * @author jhb
 * @date   2026/01/22
 */
#include "dev_module.h"

#include <dirent.h>
#include <dlfcn.h>
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "db_rpc.h"
#include "dev.h"
#include "dev_conf_parser.h"
#include "dev_main.h"
#include "errcode.h"
#include "log.h"
#include "module_ports.h"
#include "path_utils.h"

/* 前向声明 */
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data);
static void *dev_dlopen_module(const char *so_name);

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
    LOG_INFO("模块 %s (id=%u) 已添加到注册表", name, module_id);

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
            LOG_ERROR("解析 %s 失败", conf_path);
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

        /* 必须有 so */
        if (conf.so_name[0] == '\0')
        {
            LOG_WARN("模块 %s 缺少 so 字段，跳过", conf.name);
            continue;
        }

        LOG_INFO("发现模块: %s (id=%u, so=%s)", conf.name, conf.module_id, conf.so_name);

        /* dlopen 加载共享库 — constructor 自动触发
         * 使用 RTLD_LAZY 因为模块间存在交叉依赖（如 db→cfg），
         * 加载时并非所有符号都已可用，实际调用时再解析即可。
         * constructor 调用 ipc_init() 会把主线程标签改为该模块名，
         * dlopen 返回后恢复为 "dev"，保持扫描期间日志标签一致。 */
        void *dl_handle = dev_dlopen_module(conf.so_name);
        log_set_tag("dev"); /* 恢复：constructor 已把标签改为各自模块名 */
        if (!dl_handle)
        {
            LOG_ERROR("dlopen(%s) 失败: %s", conf.so_name, dlerror());
            continue;
        }

        /* 创建模块并添加到注册表 */
        dev_module_t *module = dev_add_module_to_registry(conf.module_id, conf.name);
        if (!module)
        {
            LOG_ERROR("模块 %s 注册失败", conf.name);
            dlclose(dl_handle);
            continue;
        }

        /* 设置 DEV 管理的元数据 */
        module->dl_handle = dl_handle;

        module->port = conf.port;

        loaded++;
        LOG_INFO("模块 %s 加载成功 (constructor 已执行)", conf.name);
    }

    closedir(dir);
    return loaded;
}

/**
 * @brief 优先按名称加载模块，失败后依次尝试生产路径、开发路径
 *
 * 查找顺序：
 *   1. 直接 dlopen（利用 LD_LIBRARY_PATH / RPATH 等系统机制）
 *   2. 生产环境：NN_WORK_DIR/lib/
 *   3. 生产环境：/opt/netnexus/lib/（固定路径）
 *   4. 开发环境：可执行文件相对路径 ../lib/ 和 ../../lib/
 */
static void *dev_dlopen_module(const char *so_name)
{
    void *dl_handle = dlopen(so_name, RTLD_LAZY | RTLD_GLOBAL);
    if (dl_handle)
    {
        return dl_handle;
    }

    /* 已提供绝对/相对路径时，不再做目录推断 */
    if (strchr(so_name, '/') != NULL)
    {
        return NULL;
    }

    char *so_path = NULL;

    /* 生产环境 1: 环境变量 NN_WORK_DIR */
    const char *work_dir = getenv("NN_WORK_DIR");
    if (work_dir)
    {
        so_path = g_build_filename(work_dir, "lib", so_name, NULL);
        dl_handle = dlopen(so_path, RTLD_LAZY | RTLD_GLOBAL);
        g_free(so_path);
        so_path = NULL;
        if (dl_handle)
        {
            return dl_handle;
        }
    }

    /* 开发环境: 相对于可执行文件的路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        so_path = g_build_filename(exe_dir, "..", "lib", so_name, NULL);
        dl_handle = dlopen(so_path, RTLD_LAZY | RTLD_GLOBAL);
        g_free(so_path);
        so_path = NULL;
        if (dl_handle)
        {
            return dl_handle;
        }
    }

    return NULL;
}

int32_t dev_scan_and_load_modules(void)
{
    ensure_registry_initialized();

    /* DEV 自身初始化（创建 DEV 的 IPC context + 注册到 GTree）
     * ipc_init() 内部会把调用线程标签设为 "dev"，在 dlopen 触发各模块
     * constructor 之前，日志已标记为 "dev" */
    if (dev_init_self() != ERRCODE_SUCCESS)
    {
        LOG_ERROR("Fatal: DEV module self-init failed");
        return ERRCODE_FAIL;
    }

    LOG_INFO("开始扫描并加载模块=============================================");

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
            LOG_INFO("从 NN_WORK_DIR 加载了 %d 个模块", total_loaded);
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
            LOG_INFO("从开发路径加载了 %d 个模块", total_loaded);
            return ERRCODE_SUCCESS;
        }
    }

    if (total_loaded == 0)
    {
        LOG_ERROR("未找到任何模块");
        return ERRCODE_FAIL;
    }

    LOG_INFO("结束扫描并加载模块=============================================");

    return ERRCODE_SUCCESS;
}

/* DEV 连接到所有模块 */
static gboolean dev_connect_to_module_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    dev_module_t *module = (dev_module_t *)value;
    ipc_context_t *dev_ctx = (ipc_context_t *)data;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        return FALSE;
    }

    if (module->phase >= DEV_PHASE_LOADED)
    {
        LOG_INFO("连接到模块: %s (port=%u)", module->name, module->port);
        ipc_connect(dev_ctx, module->module_id, IPC_HOST_LOCAL, module->port);
    }

    return FALSE;
}

// ============================================================================
// Phase 1: 发送 MODULE_START RPC（无 payload，各模块名称已在 ipc_init 中配置）
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

    LOG_INFO("Phase 1: 发送 MODULE_START -> %s", module->name);

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_START, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(g_dev_local->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 1: %s IPC 连接已建立", module->name);
        module->phase = DEV_PHASE_IPC_READY;
        ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 1: %s IPC 连接超时或失败", module->name);
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

    LOG_INFO("Phase 2: 发送 MODULE_CONNECT -> %s", module->name);

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_CONNECT, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(g_dev_local->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 2: %s 预留阶段完成", module->name);
        module->phase = DEV_PHASE_DB_RECOVERED;
        ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 2: %s 预留阶段超时或失败", module->name);
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

    LOG_INFO("Phase 3: 发送 MODULE_READY -> %s", module->name);

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_READY, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(g_dev_local->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        LOG_INFO("Phase 3: %s 就绪", module->name);
        module->phase = DEV_PHASE_READY;
        ipc_message_free(resp);
    }
    else
    {
        LOG_ERROR("Phase 3: %s 就绪超时或失败", module->name);
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
    LOG_INFO("开始三阶段模块初始化");
    LOG_INFO("=============================================");

    if (!g_module_registry)
    {
        LOG_INFO("没有已注册的模块");
        return ERRCODE_SUCCESS;
    }

    /* DEV 连接到所有模块 */
    LOG_INFO("DEV 连接到所有模块");
    g_tree_foreach(g_module_registry, dev_connect_to_module_callback, g_dev_local->ipc_ctx);

    /* 等待所有 IPC 连接完成握手（最多 5 秒） */
    LOG_INFO("等待 IPC 连接就绪...");
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
                if (!ipc_is_connected(g_dev_local->ipc_ctx, m->module_id))
                {
                    all_connected = 0;
                    if (retry % 10 == 9) /* 每 1 秒记录一次等待中的模块 */
                    {
                        LOG_WARN("等待 IPC 连接: %s (id=0x%08X) 未就绪", m->name, m->module_id);
                    }
                }
            }
        }
        g_list_free(wait_modules);

        if (all_connected)
        {
            LOG_INFO("所有 IPC 连接就绪 (耗时约 %d ms)", retry * 100);
            break;
        }
        usleep(100000); /* 100ms */
    }

    if (!all_connected)
    {
        LOG_WARN("等待超时（5 秒），部分模块 IPC 连接未就绪，继续初始化");
        /* 打印每个模块的最终连接状态 */
        GList *mods = NULL;
        g_tree_foreach(g_module_registry, collect_module_callback, &mods);
        for (GList *l = mods; l != NULL; l = l->next)
        {
            dev_module_t *m = (dev_module_t *)l->data;
            if (m->module_id != DEV_MODULE_ID_DEV)
            {
                int connected = ipc_is_connected(g_dev_local->ipc_ctx, m->module_id);
                LOG_WARN("  模块 %s (id=0x%08X): %s", m->name, m->module_id, connected ? "已连接" : "未连接");
            }
        }
        g_list_free(mods);
    }

    /* Phase 1: 发送 MODULE_START — 模块建立 IPC 连接 */
    LOG_INFO("=== Phase 1: MODULE_START (IPC 建立) ===");
    g_tree_foreach(g_module_registry, phase1_start_callback, &failed_count);

    /* Phase 2: 发送 MODULE_CONNECT — 预留（DB 恢复） */
    LOG_INFO("=== Phase 2: MODULE_CONNECT (预留) ===");
    g_tree_foreach(g_module_registry, phase2_connect_callback, &failed_count);
    LOG_INFO("Phase 2: 所有模块预留阶段完成");

    /* Phase 3: 发送 MODULE_READY — 预留（CFG 加载 XML） */
    LOG_INFO("=== Phase 3: MODULE_READY (预留) ===");
    g_tree_foreach(g_module_registry, phase3_ready_callback, &failed_count);

    LOG_INFO("=============================================");
    LOG_INFO("三阶段初始化完成 (失败: %d)", failed_count);
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
    LOG_INFO("==================== 清理模块 ====================");

    if (!g_module_registry)
    {
        LOG_INFO("没有需要清理的模块");
        return;
    }

    /* 收集模块，g_list_prepend 得到逆序（高 ID 先） */
    GList *modules = NULL;
    g_tree_foreach(g_module_registry, collect_module_callback, &modules);

    /* Step 1: 逆序发送 MODULE_SHUTDOWN RPC */
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_t *module = (dev_module_t *)l->data;

        if (module->module_id == DEV_MODULE_ID_DEV)
        {
            continue;
        }

        if (module->phase >= DEV_PHASE_LOADED)
        {
            LOG_INFO("发送 MODULE_SHUTDOWN -> %s", module->name);

            ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, NULL, 0, NULL);

            ipc_message_t *resp = ipc_query(g_dev_local->ipc_ctx, module->module_id, req, 3000);
            if (resp)
            {
                ipc_message_free(resp);
            }
        }
    }

    /* Step 2: 逆序销毁每个模块的 IPC context */
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_t *module = (dev_module_t *)l->data;

        if (module->module_id == DEV_MODULE_ID_DEV)
        {
            continue;
        }

        LOG_INFO("销毁模块: %s", module->name);

        if (module->dl_handle)
        {
            dlclose(module->dl_handle);
            module->dl_handle = NULL;
        }

        g_free(module);
    }

    /* Step 3: 销毁 DEV */
    LOG_INFO("销毁 DEV");
    dev_module_t *dev_self = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (dev_self)
    {
        g_free(dev_self);
    }

    g_list_free(modules);

    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    LOG_INFO("模块清理完成");
}
