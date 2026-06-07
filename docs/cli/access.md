# ACCESS CLI 文档

ACCESS 模块（module-id: 13）负责本地 console、VTY/telnet line 配置、分页控制和交互式 bash。

## 用户视图命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `bash` | user | 进入交互式 bash，退出 bash 后返回 CLI |
| `terminal length 0` | user | 当前会话关闭分页 |
| `no terminal length 0` | user | 恢复当前会话分页默认行为 |

## 线路配置

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `line vty <first-line> <last-line>` | config | 进入 VTY line 视图，范围 0-4 |
| `line console 0` | config | 进入 console line 视图 |
| `transport input telnet` | line | 允许 telnet 访问 |
| `transport input ssh` | line | SSH 占位命令，当前未实现 SSH 服务 |
| `transport input all` | line | 允许所有已实现传输 |
| `transport input none` | line | 禁止远程登录 |
| `no transport input` | line | 禁止远程登录 |
| `telnet server enable` | config | 启用 telnet server，监听 TCP 23 |
| `no telnet server enable` | config | 关闭 telnet server |

默认管理入口是本地 console socket；telnet/vty 需要同时开启服务器和线路传输。

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show line` | global | 显示 console/vty line 类型、transport 和状态 |
