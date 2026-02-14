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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cli.h"
#include "db_rpc.h"
#include "dev.h"
#include "dev_conf_parser.h"
#include "errcode.h"
#include "path_utils.h"

/* 前向声明 */
static gboolean collect_module_callback(gpointer key, gpointer value, gpointer data);

/* 全局模块注册表（GLib tree: id -> dev_module_t*） */
static GTree *g_module_registry = NULL;

/* 全局关闭标志 */
static volatile sig_atomic_t g_shutdown_requested = 0;

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
// 注册接口
// ============================================================================

void dev_register_module_inner(uint32_t id, const char *name, ipc_msg_handler_fn handler)
{
    ensure_registry_initialized();

    dev_module_t *module = (dev_module_t *)g_malloc0(sizeof(dev_module_t));

    module->module_id = id;
    strlcpy(module->name, name, sizeof(module->name));
    module->msg_handler = handler;
    module->phase = DEV_PHASE_REGISTERED;

    g_tree_insert(g_module_registry, GUINT_TO_POINTER(module->module_id), module);
}

// ============================================================================
// Shutdown 控制
// ============================================================================

void dev_request_shutdown_inner(void)
{
    g_shutdown_requested = 1;
}

int dev_shutdown_requested_inner(void)
{
    return g_shutdown_requested;
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



// ============================================================================
// 自动发现 commands.xml 并注册到 cfg_xml_registry
// ============================================================================

static gboolean discover_xml_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    dev_module_t *module = (dev_module_t *)value;

    char xml_path[256];
    if (resolve_xml_path(module->name, xml_path, sizeof(xml_path)) == 0)
    {
        module->xml_path = g_strdup(xml_path);
        cfg_register_module_xml(module->module_id, xml_path);
        printf("[dev] 发现 XML: %s -> %s\n", module->name, xml_path);
    }

    return FALSE;
}

static void dev_discover_xml_paths(void)
{
    printf("[dev] 自动发现 commands.xml...\n");
    g_tree_foreach(g_module_registry, discover_xml_callback, NULL);
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

        /* 检查子目录下是否有 module.conf */
        char conf_path[PATH_MAX];
        snprintf(conf_path, sizeof(conf_path), "%s/%s/module.conf", base_dir, entry->d_name);

        struct stat st;
        if (stat(conf_path, &st) != 0)
        {
            continue;
        }

        /* 解析 module.conf */
        dev_module_conf_t conf;
        if (dev_conf_parse(conf_path, &conf) != 0)
        {
            fprintf(stderr, "[dev] 解析 %s 失败\n", conf_path);
            continue;
        }

        /* 跳过 DEV 模块（由 dev_init_self 单独处理） */
        if (conf.module_id == DEV_MODULE_ID_DEV)
        {
            continue;
        }

        /* 必须有 entry 和 so */
        if (conf.entry_func[0] == '\0' || conf.apiso_name[0] == '\0')
        {
            printf("[dev] 模块 %s 缺少 entry 或 so 字段，跳过\n", conf.name);
            continue;
        }

        printf("[dev] 发现模块: %s (id=%u, so=%s, entry=%s)\n",
               conf.name, conf.module_id, conf.apiso_name, conf.entry_func);

        /* dlopen 加载共享库 */
        void *dl_handle = dlopen(conf.apiso_name, RTLD_NOW | RTLD_GLOBAL);
        if (!dl_handle)
        {
            fprintf(stderr, "[dev] dlopen(%s) 失败: %s\n", conf.apiso_name, dlerror());
            continue;
        }

        /* dlsym 查找入口函数 */
        module_entry_fn entry_fn = (module_entry_fn)dlsym(dl_handle, conf.entry_func);
        if (!entry_fn)
        {
            fprintf(stderr, "[dev] dlsym(%s) 失败: %s\n", conf.entry_func, dlerror());
            dlclose(dl_handle);
            continue;
        }

        /* 注册模块到 GTree（handler 为 NULL，由入口函数内部设置） */
        dev_register_module_inner(conf.module_id, conf.name, NULL);

        /* 查找已注册的 module 结构体 */
        dev_module_t *module =
            (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(conf.module_id));
        if (!module)
        {
            fprintf(stderr, "[dev] 模块 %s 注册失败\n", conf.name);
            dlclose(dl_handle);
            continue;
        }

        module->dl_handle = dl_handle;

        /* 设置 db_name */
        if (conf.db_name[0] != '\0')
        {
            module->db_name = g_strdup(conf.db_name);
            printf("[dev] 模块 %s 需要数据库: %s\n", conf.name, module->db_name);
        }

        /* 调用入口函数，创建 IPC 上下文 */
        printf("[dev] 调用入口函数: %s()\n", conf.entry_func);
        ipc_context_t *ipc_ctx = entry_fn();
        if (!ipc_ctx)
        {
            fprintf(stderr, "[dev] 模块 %s 入口函数返回 NULL\n", conf.name);
            continue;
        }

        module->ipc_ctx = ipc_ctx;
        module->phase = DEV_PHASE_IPC_CREATED;
        loaded++;

        printf("[dev] 模块 %s 加载成功 (IPC 已创建)\n", conf.name);
    }

    closedir(dir);
    return loaded;
}

int32_t dev_scan_and_load_modules(void)
{
    ensure_registry_initialized();

    printf("[dev] =============================================\n");
    printf("[dev] 开始扫描并加载模块\n");
    printf("[dev] =============================================\n");

    int total_loaded = 0;

    /* 优先级 1: 环境变量 RESOURCES_DIR */
    const char *resources_dir = getenv("RESOURCES_DIR");
    if (resources_dir)
    {
        total_loaded = dev_scan_dir_for_modules(resources_dir);
        if (total_loaded > 0)
        {
            printf("[dev] 从 RESOURCES_DIR 加载了 %d 个模块\n", total_loaded);
            return ERRCODE_SUCCESS;
        }
    }

    /* 优先级 2: 生产路径 */
    total_loaded = dev_scan_dir_for_modules("/opt/netnexus/resources");
    if (total_loaded > 0)
    {
        printf("[dev] 从生产路径加载了 %d 个模块\n", total_loaded);
        return ERRCODE_SUCCESS;
    }

    /* 优先级 3: 相对于可执行文件的开发路径 */
    char exe_dir[PATH_MAX];
    if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0)
    {
        char dev_path[PATH_MAX];

        snprintf(dev_path, sizeof(dev_path), "%s/../src", exe_dir);
        total_loaded = dev_scan_dir_for_modules(dev_path);
        if (total_loaded > 0)
        {
            printf("[dev] 从开发路径加载了 %d 个模块\n", total_loaded);
            return ERRCODE_SUCCESS;
        }

        snprintf(dev_path, sizeof(dev_path), "%s/../../src", exe_dir);
        total_loaded = dev_scan_dir_for_modules(dev_path);
        if (total_loaded > 0)
        {
            printf("[dev] 从开发路径加载了 %d 个模块\n", total_loaded);
            return ERRCODE_SUCCESS;
        }
    }

    if (total_loaded == 0)
    {
        fprintf(stderr, "[dev] 未找到任何模块\n");
        return ERRCODE_FAIL;
    }

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

    if (module->phase >= DEV_PHASE_IPC_CREATED)
    {
        printf("[dev] 连接到模块: %s\n", module->name);
        ipc_connect(dev_ctx, module->module_id);
    }

    return FALSE;
}

// ============================================================================
// Phase 1: 发送 MODULE_START RPC
// ============================================================================

static gboolean phase1_start_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    int32_t *failed_count = (int32_t *)data;
    dev_module_t *module = (dev_module_t *)value;

    if (module->module_id == DEV_MODULE_ID_DEV)
    {
        module->phase = DEV_PHASE_STARTED;
        return FALSE;
    }

    if (module->phase < DEV_PHASE_IPC_CREATED)
    {
        return FALSE;
    }

    printf("[dev] Phase 1: 发送 MODULE_START -> %s\n", module->name);

    /* 通过 DEV 的 IPC ctx 发送 RPC 到目标模块 */
    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (!dev_mod || !dev_mod->ipc_ctx)
    {
        (*failed_count)++;
        return FALSE;
    }

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_START, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(dev_mod->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        printf("[dev] Phase 1: %s 启动成功\n", module->name);
        module->phase = DEV_PHASE_STARTED;
        ipc_message_free(resp);
    }
    else
    {
        fprintf(stderr, "[dev] Phase 1: %s 启动超时或失败\n", module->name);
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
        module->phase = DEV_PHASE_CONNECTED;
        return FALSE;
    }

    if (module->phase < DEV_PHASE_STARTED)
    {
        return FALSE;
    }

    printf("[dev] Phase 2: 发送 MODULE_CONNECT -> %s\n", module->name);

    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (!dev_mod || !dev_mod->ipc_ctx)
    {
        (*failed_count)++;
        return FALSE;
    }

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_CONNECT, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(dev_mod->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        printf("[dev] Phase 2: %s 建连成功\n", module->name);
        module->phase = DEV_PHASE_CONNECTED;
        ipc_message_free(resp);
    }
    else
    {
        fprintf(stderr, "[dev] Phase 2: %s 建连超时或失败\n", module->name);
        (*failed_count)++;
    }

    return FALSE;
}

// ============================================================================
// 创建数据库（Phase 2 之后、Phase 3 之前）
// ============================================================================

static gboolean create_db_callback(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    dev_module_t *module = (dev_module_t *)value;

    if (!module->db_name)
    {
        return FALSE;
    }

    /* 使用 DEV 的 IPC ctx 调用 db_rpc_create_db */
    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (!dev_mod || !dev_mod->ipc_ctx)
    {
        return FALSE;
    }

    printf("[dev] 创建数据库: %s (module=%u)\n", module->db_name, module->module_id);
    if (db_rpc_create_db(dev_mod->ipc_ctx, module->db_name, module->module_id) != ERRCODE_SUCCESS)
    {
        fprintf(stderr, "[dev] 创建数据库失败: %s\n", module->db_name);
    }

    return FALSE;
}

static void dev_create_databases(void)
{
    printf("[dev] 创建模块数据库...\n");
    g_tree_foreach(g_module_registry, create_db_callback, NULL);
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

    if (module->phase < DEV_PHASE_CONNECTED)
    {
        return FALSE;
    }

    printf("[dev] Phase 3: 发送 MODULE_READY -> %s\n", module->name);

    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (!dev_mod || !dev_mod->ipc_ctx)
    {
        (*failed_count)++;
        return FALSE;
    }

    ipc_message_t *req =
        ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_READY, DEV_MODULE_ID_DEV, module->module_id, 0, NULL, 0, NULL);

    ipc_message_t *resp = ipc_query(dev_mod->ipc_ctx, module->module_id, req, 5000);
    if (resp)
    {
        printf("[dev] Phase 3: %s 就绪\n", module->name);
        module->phase = DEV_PHASE_READY;
        ipc_message_free(resp);
    }
    else
    {
        fprintf(stderr, "[dev] Phase 3: %s 就绪超时或失败\n", module->name);
        (*failed_count)++;
    }

    return FALSE;
}

// ============================================================================
// 三阶段初始化主流程
// ============================================================================

int32_t dev_init_all_modules(void)
{
    int32_t failed_count = 0;

    printf("[dev] =============================================\n");
    printf("[dev] 开始三阶段模块初始化\n");
    printf("[dev] =============================================\n");

    if (!g_module_registry)
    {
        printf("[dev] 没有已注册的模块\n");
        return ERRCODE_SUCCESS;
    }

    /* Step 1: 自动发现 commands.xml，注册到 cfg_xml_registry */
    printf("[dev] Step 1: 自动发现 XML\n");
    dev_discover_xml_paths();

    /* DEV 连接到所有模块 */
    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));
    if (dev_mod && dev_mod->ipc_ctx)
    {
        printf("[dev] DEV 连接到所有模块\n");
        g_tree_foreach(g_module_registry, dev_connect_to_module_callback, dev_mod->ipc_ctx);
    }

    /* 等待所有 IPC 连接完成握手 */
    if (dev_mod && dev_mod->ipc_ctx)
    {
        printf("[dev] 等待 IPC 连接就绪...\n");
        for (int retry = 0; retry < 50; retry++)
        {
            int all_connected = 1;
            GList *wait_modules = NULL;
            g_tree_foreach(g_module_registry, collect_module_callback, &wait_modules);

            for (GList *l = wait_modules; l != NULL; l = l->next)
            {
                dev_module_t *m = (dev_module_t *)l->data;
                if (m->module_id != DEV_MODULE_ID_DEV && m->phase >= DEV_PHASE_IPC_CREATED)
                {
                    if (!ipc_is_connected(dev_mod->ipc_ctx, m->module_id))
                    {
                        all_connected = 0;
                        break;
                    }
                }
            }
            g_list_free(wait_modules);

            if (all_connected)
            {
                printf("[dev] 所有 IPC 连接就绪\n");
                break;
            }
            usleep(100000); /* 100ms */
        }
    }

    /* Phase 1: 发送 MODULE_START */
    printf("\n[dev] === Phase 1: MODULE_START ===\n");
    g_tree_foreach(g_module_registry, phase1_start_callback, &failed_count);

    /* Phase 2: 发送 MODULE_CONNECT */
    printf("\n[dev] === Phase 2: MODULE_CONNECT ===\n");
    g_tree_foreach(g_module_registry, phase2_connect_callback, &failed_count);
    printf("[dev] Phase 2: 所有模块建连完成\n");

    /* 创建数据库（Phase 2 之后） */
    printf("\n[dev] === 创建数据库 ===\n");
    dev_create_databases();

    /* Phase 3: 发送 MODULE_READY */
    printf("\n[dev] === Phase 3: MODULE_READY ===\n");
    g_tree_foreach(g_module_registry, phase3_ready_callback, &failed_count);

    printf("\n[dev] =============================================\n");
    printf("[dev] 三阶段初始化完成 (失败: %d)\n", failed_count);
    printf("[dev] =============================================\n\n");

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
    printf("[dev] 清理模块:\n");
    printf("====================\n");

    if (!g_module_registry)
    {
        printf("[dev] 没有需要清理的模块\n");
        return;
    }

    /* 收集模块，g_list_prepend 得到逆序（高 ID 先） */
    GList *modules = NULL;
    g_tree_foreach(g_module_registry, collect_module_callback, &modules);

    /* 获取 DEV 的 IPC ctx 用于发送 shutdown */
    dev_module_t *dev_mod = (dev_module_t *)g_tree_lookup(g_module_registry, GUINT_TO_POINTER(DEV_MODULE_ID_DEV));

    /* Step 1: 逆序发送 MODULE_SHUTDOWN RPC */
    for (GList *l = modules; l != NULL; l = l->next)
    {
        dev_module_t *module = (dev_module_t *)l->data;

        if (module->module_id == DEV_MODULE_ID_DEV)
        {
            continue;
        }

        if (module->phase >= DEV_PHASE_STARTED && dev_mod && dev_mod->ipc_ctx)
        {
            printf("[dev] 发送 MODULE_SHUTDOWN -> %s\n", module->name);

            ipc_message_t *req = ipc_message_create(IPC_MSG_TYPE_DEV_MODULE_SHUTDOWN, DEV_MODULE_ID_DEV,
                                                    module->module_id, 0, NULL, 0, NULL);

            ipc_message_t *resp = ipc_query(dev_mod->ipc_ctx, module->module_id, req, 3000);
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

        printf("[dev] 销毁 IPC: %s\n", module->name);
        printf("[dev] ============================================\n");

        if (module->ipc_ctx)
        {
            ipc_destroy(module->ipc_ctx);
            module->ipc_ctx = NULL;
        }

        if (module->dl_handle)
        {
            dlclose(module->dl_handle);
            module->dl_handle = NULL;
        }

        g_free(module->db_name);
        g_free(module->xml_path);
        g_free(module);

        printf("[dev] ============================================\n");
    }

    /* Step 3: 销毁 DEV 自己的 IPC context */
    if (dev_mod)
    {
        printf("[dev] 销毁 DEV IPC\n");
        if (dev_mod->ipc_ctx)
        {
            ipc_destroy(dev_mod->ipc_ctx);
            dev_mod->ipc_ctx = NULL;
        }
        g_free(dev_mod->db_name);
        g_free(dev_mod->xml_path);
        g_free(dev_mod);
    }

    g_list_free(modules);

    g_tree_destroy(g_module_registry);
    g_module_registry = NULL;

    printf("[dev] 模块清理完成\n");
}
