# CLI 核心命令文档

CLI 模块（module-id: 3）负责命令树、视图切换、命令历史、当前配置展示、上下文查看和配置差异展示。业务命令由各模块的 `resources/commands.xml` 注册到同一命令树。

## 1. 导航命令

### 1.1 `config`
从用户视图进入配置视图。

- **用法**：`config`
- **视图**：`user`
- **视图切换**：切换到 `config` 视图。

### 1.2 `exit`
退出当前视图并返回父视图。如果已经在顶层用户视图，则关闭当前 CLI 会话。

- **用法**：`exit`
- **视图**：`global`（所有视图可用）

### 1.3 `end`
从任意嵌套视图直接返回用户视图，并清空上下文数据。

- **用法**：`end`
- **视图**：`global`（所有视图可用）

## 2. 查看命令

### 2.1 `show cli command-info`
显示所有视图、所有模块已注册的 CLI 命令。

- **用法**：`show cli command-info`
- **视图**：`global`（所有视图可用）
- **输出**：包含视图、模块、命令的表格。

### 2.2 `show cli history`
显示全局命令执行历史，包括时间戳和客户端 IP。

- **用法**：`show cli history`
- **视图**：`global`（所有视图可用）
- **输出**：包含序号、时间、命令、客户端 IP 的表格。

### 2.3 `show current-configuration`
显示从各业务模块收集的当前运行配置。

- **用法**：`show current-configuration`
- **视图**：`global`（所有视图可用）

### 2.4 `show cli context`
显示当前会话上下文变量，即 TLV 编码的视图状态。

- **用法**：`show cli context`
- **视图**：`global`（所有视图可用）
- **输出**：包含 ctx-id、类型和值的表格。

### 2.5 `show this`
显示当前视图上下文对应的配置。

- **用法**：`show this`
- **视图**：支持该命令的配置视图

### 2.6 `show cli client`
显示当前已连接的 CLI 客户端。

- **用法**：`show cli client`
- **视图**：`global`

### 2.7 `show configuration difference current-configuration <snapshot-name>`

按完整父视图路径比较当前运行配置和命名快照。重复命令使用 multiset 语义，
不同 VRF/AF 下的同名命令不会互相抵消。这是回滚前唯一的只读预览入口：
共同父视图用空格标记，目标快照独有项用 `+`（回滚会新增），当前运行配置
独有项用 `-`（回滚会删除）。

- **用法**：`show configuration difference current-configuration <snapshot-name>`
- **视图**：`global`
- **参数限制**：只接受 `data/configs` 下的命名快照，可带或不带 `.cfg` 后缀；
  不接受绝对路径或包含 `/` 的路径。
- **边界**：该命令只比较配置树，不校验每条差异是否存在安全逆命令；
  真正执行回滚时会在修改 running 前完成正向和补偿路径预检。

### 2.8 `rollback configuration <snapshot-name>`

执行已经完整预检的回滚计划。流程为：

1. 校验快照名、大小、capture 状态和 SHA-256。
2. 完整采集 running BDR，并生成 `current -> target` 计划。
3. 预检 `target -> current` 补偿路径。
4. 执行前再次采集 running；若配置已变化则拒绝旧计划。
5. 逐条校验期望视图深度，执行后重新采集并与 target 配置树比较。
6. 执行或验证失败时，从实际 running 状态重算补偿计划，恢复并验证原配置。

执行前可用
`show configuration difference current-configuration <snapshot-name>` 预览层级差异。
内部生成的 `undo/add/exit` 执行计划不再作为独立 CLI 输出。

回滚不是跨进程的分布式事务；若业务模块故障导致补偿也失败，CLI 会明确报告
running 可能处于部分状态，不会输出成功。

## 3. 系统命令

`bash` 现在由 ACCESS line 模块提供，见 `docs/cli/access.md`。
