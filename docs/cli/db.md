# DB CLI 文档

本文档描述 DB 模块（module-id: 2）提供的数据库查看和配置快照命令。

## 1. 查看命令（global 视图）

### 1.1 `show db table-list`
显示当前运行数据库中的用户表列表。

- **用法**：`show db table-list`
- **视图**：`global`（所有视图可用）

### 1.2 `show db table-field <table-name>`
显示指定表的结构，包括列 ID、名称、类型、非空约束和主键状态。

- **用法**：`show db table-field <table-name>`
- **视图**：`global`（所有视图可用）
- **参数**：
    - `<table-name>`：表名，类型为 `dynamic(string(1-63))`，支持通过现有表名补全。

### 1.3 `show db table-data <table-name>`
显示指定表的全部数据行。

- **用法**：`show db table-data <table-name>`
- **视图**：`global`（所有视图可用）
- **参数**：
    - `<table-name>`：表名，类型为 `dynamic(string(1-63))`，支持通过现有表名补全。

## 2. 配置快照命令

### 2.1 `save configuration [name]`
将当前运行配置保存为命名快照。

- **用法**：`save configuration [name]`
- **输出文件**：
    - `data/configs/<name>.db`：SQLite 运行数据库快照。
    - `data/configs/<name>.cfg`：来自 `show current-configuration` 的层级 BDR 文本。
    - `data/configs/<name>.meta`：版本、`format=bdr-indent-v1`、完整采集标记和
      CFG SHA-256。
- **完整性**：任一已连接模块 BDR 超时、断开或分片异常时保存失败，不再发布部分快照。
- **默认名称**：当前 startup 名称；未选择 startup 配置时为 `startup`。
- **保存条件**：`.db` 和 `.cfg` 都写入成功才算保存成功。

### 2.2 `startup configuration <name> {db|cfg}`
选择下次冷启动使用的保存配置。

- **用法**：
    - `startup configuration <name> db`：将 `data/configs/<name>.db` 恢复为 `running.db`。
    - `startup configuration <name> cfg`：以空 `running.db` 启动，DEV 就绪后回放 `data/configs/<name>.cfg`。
      回放前会完成文件完整性、层级和整棵命令树预检；配置行缩进变浅时由回放器
      执行真实 `exit` 命令，不再直接修改内部 view 指针。
- **指针文件**：`data/startup.cfg` 保存 `<mode> <name>`。

### 2.3 `show startup configuration`
显示当前选择的 startup 快照名称和模式。

### 2.4 `show configuration replay-failures`
显示最近一次冷启动时 `cfg` 回放失败的命令。成功回放、`db` 启动模式和出厂启动都会清空失败列表。
