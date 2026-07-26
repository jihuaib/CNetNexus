# CFG 启动与配置回滚设计

## 1. 现状与目标

系统并非只能固定 DB 启动，已经有两种启动选择：

```text
startup configuration <name> db
startup configuration <name> cfg
```

`db` 在 DB 进程冷启动时恢复 SQLite 快照；`cfg` 先创建空 running DB，待 DEV
READY 后由 CLI 回放 BDR 文本。本次设计补齐了 CFG 的视图退出、层级比较、逆命令
计划、在线回滚和失败语义。

## 2. CFG 文件与视图退出

当前保存格式记为 `bdr-indent-v1`。命令的前导缩进表示父视图，`!` 仍兼容旧快照。
为避免要求每个业务模块的 BDR builder 都重复拼接导航命令，保存文件暂时保持声明式
配置；回放器在缩进回退和 EOF 时通过命令树执行真实 `exit`。这与“每个视图末尾写
exit”的运行效果相同，同时不会把操作命令混入 `show current-configuration`。

带显式 `exit` 的文件也兼容，推荐格式为：

```text
bgp 100
 af ipv4-unicast
  import-route connected
  exit
 exit
route static ipv4 192.0.2.0 24 interface null0
```

`exit` 位于被退出视图的 body 深度。空视图也可以写入口后紧跟 `exit`。配置模型把
`config`、`exit`、`end` 当作控制行，不参与配置差异。

## 3. 层级差异模型

配置解析成有序树，节点身份是：

```text
完整父视图路径 + 规范化命令
```

同一父节点下使用 FIFO multiset 配对，保留重复项；跨 VRF、AF、接口的同名命令
绝不配对。只有顺序变化不构成配置差异。缩进跳级、最大深度越界和超长命令在任何
下发之前失败，并报告原文件行号。

## 4. Undo 与 Add 计划

每一层按以下顺序生成：

1. current 独有项按逆序生成 `undo`，深层配置先撤销。
2. 共同视图仅在子树有差异时进入，结束后自动添加 `exit`。
3. target 独有项按 target 顺序生成 `add`，父视图先于子命令。
4. 新增空视图同样生成“入口 + exit”。

逆命令求解顺序：

1. 优先使用命令 XML 的声明式 `<inverse>`。
2. 原命令以 `no ` 开头时尝试移除 `no`。
3. 否则从 `no <完整原命令>` 开始逐步去掉尾部参数，例如
   `router-id 1.1.1.1 -> no router-id`。
4. 每个候选都必须在原父视图的命令树中完整匹配；自动推导要求 module/group
   一致，声明式 inverse 允许同模块跨 group。

参数化声明示例：

```xml
<command>
    <expression>...</expression>
    <views>...</views>
    <inverse>no foo {cfg:1}</inverse>
</command>
```

若固定视图入口不可删除，可以进入视图逐条撤销子配置；无法删除的空视图和没有
安全逆命令的叶子会使整份计划在执行前失败。

## 5. 执行与失败语义

`show configuration difference current-configuration <name>` 是唯一只读预览入口：
`+` 表示目标快照独有、回滚时会新增，`-` 表示 running 独有、回滚时会删除。
该命令只比较配置树；`rollback configuration <name>` 才会在修改 running 前
预检正向执行与反向补偿路径：

1. 限制目标为 `data/configs` 下的安全快照名，验证 capture 状态与 SHA-256。
2. 要求全部常驻配置模块，以及 revive-table 中确有配置的按需模块在线并完整返回
   running BDR；缺少模块时拒绝操作，不能把部分结果标记为完整。
3. 同时预检正向计划和反向补偿路径。
4. 执行前二次采集，检测计划期间的配置变化。
5. 每一步检查执行前后的视图深度；最终重新采集并做配置树相等验证。
6. 失败时基于实际 running 状态生成 `actual -> original` 补偿计划，并再次验证。

系统目前没有跨模块的 prepare/commit 协议，因此在线回滚属于“完整预检、顺序执行、
后验验证、补偿”，不是严格分布式原子事务。另一个仍需后续解决的问题是 BDR
采集和 SQLite backup 之间没有共享 revision/freeze，极端并发写入下 `.cfg` 与
`.db` 可能来自相邻状态。

## 6. 启动可靠性

- CFG 回放前验证非空、16 MiB 上限、capture 完整标记和 SHA-256。
- 整份配置先完成层级与命令树预检，再开始修改模块。
- 只接受当前配置视图自身的命令，不允许借全局命令树执行 `reboot`、`show`、
  `terminal` 等运维命令；控制命令大小写与实际 CLI 匹配规则一致。
- 视图退出必须走真实 `exit` 命令。
- 内部回放会通过专用 RPC 应用 ACCESS 的 `line vty`、`transport input` 和
  `telnet server` 配置，避免无真实终端时静默丢失。
- 部分成功后不再盲目重放整份文件；失败记录写入
  `data/startup-replay-failures.log`。

## 7. 通用 CI 验证

验证统一放在 `scripts/ci/modules`，不在 `src/cli` 下维护独立测试框架：

- `cli/n2-l1-g1/show_conf_diff.py`：统一层级 diff 预览、声明式 inverse、
  无逆命令回滚预检、路径限制、实际 rollback 和回滚后无差异。
- `db/n1-l0-g1/cfg_startup_replay.py`：显式 exit 的 cfg 冷启动、错误缩进、
  根视图 exit、全局运维命令和大小写不一致的全量预检。
- `db/n1-l0-g1/db_config_management.py`：db/cfg 启动切换、版本回退、快照文件
  以及配置模块断开时拒绝不完整捕获。
