# DEV CLI 文档

DEV 模块（module-id: 1）负责 supervisor 侧的设备信息、模块生命周期、IPC 状态、日志级别、基础 ping 和少量文件查看命令。

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show version` | global | 显示版本、编译信息、ASAN 状态、日志级别和进程信息 |
| `show dev modules` | global | 显示已注册模块的 ID、名称、阶段、端口和 IPC 状态 |
| `show dev subscribe` | global | 显示 DEV 视角的模块订阅关系 |
| `show dev ipc <module-name>` | global | 显示指定模块的 IPC 连接详情 |

`<module-name>` 使用已注册模块名，例如 `if`、`route`、`bgp`、`lldp`。

## 配置命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `sysname <hostname>` | config | 设置系统名称 |
| `no sysname` | config | 恢复默认系统名称 |
| `dev log log-level {debug|info|warn|error}` | config | 设置运行时日志级别 |
| `syslog server <server> [port <port>]` | config | 配置远端 syslog 服务器，端口默认 514 |
| `no syslog server` | config | 关闭远端 syslog 上报 |
| `snmp trap server <server> [port <port>]` | config | 配置 SNMP trap 接收端，端口默认 162（命令由 SNMP 模块注册和处理） |
| `no snmp trap server` | config | 关闭 SNMP trap 上报（命令由 SNMP 模块注册和处理） |

## 进程命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `reboot` | global | 重启 NetNexus 软件栈 |
| `process reboot <module-name>` | global | 终止并重启指定模块进程，保留 DB 配置 |
| `process start <module-name>` | global | 启动已停止的模块进程 |
| `process stop <module-name>` | global | 优雅停止指定模块进程，不自动拉起 |
| `dev swap-image <image-name>` | global | 从指定 Docker image 替换 bin/lib 后重启 |

## 网络命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ping <ipv4-address> [vrf <vrf-name>]` | global | IPv4 ping，可指定 VRF |
| `ping <ipv4-address> -a <src-ipv4> [vrf <vrf-name>]` | global | 指定源地址的 IPv4 ping |
| `ping ipv6 <ipv6-address> [vrf <vrf-name>]` | global | IPv6 ping，可指定 VRF |
| `ping ipv6 <ipv6-address> -a <src-ipv6> [vrf <vrf-name>]` | global | 指定源地址的 IPv6 ping |
| `ping mpls ipv4 <ipv4-prefix>` | global | 按 IPv4 FEC 触发 MPLS tunnel ping，例如 `3.3.3.3/32` |
| `ping mpls ipv4 <ipv4-prefix> -a <src-ipv4>` | global | 指定源地址的 MPLS IPv4 FEC ping |

MPLS ping 依赖运行环境具备 `cap_net_raw` 和 Linux MPLS 内核模块。

## 文件命令

这些命令在 user view 可用，路径限制在运行工作目录内。

| 命令 | 说明 |
| --- | --- |
| `pwd` | 显示当前工作目录 |
| `ls` | 列出当前工作目录文件 |
| `cd` | 回到工作目录根 |
| `cd <path>` | 切换到工作目录内的子路径 |
| `more <file>` | 查看文件内容 |
