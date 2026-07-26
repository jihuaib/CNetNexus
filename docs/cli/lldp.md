# LLDP CLI 文档

LLDP 模块（module-id: 14）提供全局 LLDP 开关、全局 timer/hold 配置、接口 admin status、端口描述、邻居和统计展示。

## 全局命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `lldp` | config | 启用 LLDP |
| `no lldp` | config | 关闭 LLDP |
| `lldp timer <value>` | config | 配置发送间隔，范围 5-32768 秒 |
| `no lldp timer` | config | 恢复默认发送间隔 |
| `lldp hold-multiplier <value>` | config | 配置 hold multiplier，范围 2-10 |
| `no lldp hold-multiplier` | config | 恢复默认 hold multiplier |

## 接口命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `lldp enable` | if, if-loop | 恢复接口隐式启用状态，并清除显式关闭 override |
| `no lldp enable` | if, if-loop | 在接口显式关闭 LLDP（持久化 negative override） |
| `lldp admin-status txrx` | if, if-loop | 接口收发 LLDP |
| `lldp admin-status rxonly` | if, if-loop | 接口仅接收 LLDP |
| `lldp admin-status txonly` | if, if-loop | 接口仅发送 LLDP |
| `lldp admin-status disabled` | if, if-loop | 接口禁用 LLDP 收发 |
| `no lldp admin-status` | if, if-loop | 恢复接口 admin status |
| `lldp port-description <text>` | if, if-loop | 配置端口描述 |
| `no lldp port-description` | if, if-loop | 删除端口描述 |

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show lldp` | global | 显示 LLDP 全局状态 |
| `show lldp interface` | global | 显示 LLDP 接口状态 |
| `show lldp neighbors` | global | 显示 LLDP 邻居概要 |
| `show lldp neighbors detail` | global | 显示 LLDP 邻居详情 |
| `show lldp statistics` | global | 显示 LLDP 统计 |
