# FIB CLI 文档

FIB 模块（module-id: 10）负责从 Route 模块接收转发表项、同步 Linux OS FIB，并维护 MPLS label/NHLFE 相关状态。

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show fib ipv4` | global | 显示 NetNexus IPv4 FIB |
| `show fib ipv6` | global | 显示 NetNexus IPv6 FIB |
| `show fib ipv4 <prefix> <prefix-length>` | global | 查询指定 IPv4 FIB 前缀 |
| `show fib ipv6 <prefix> <prefix-length>` | global | 查询指定 IPv6 FIB 前缀 |
| `show fib os ipv4` | global | 显示 OS 侧 IPv4 FIB |
| `show fib os ipv6` | global | 显示 OS 侧 IPv6 FIB |
| `show fib os ipv4 <prefix> <prefix-length>` | global | 查询 OS 侧 IPv4 FIB |
| `show fib os ipv6 <prefix> <prefix-length>` | global | 查询 OS 侧 IPv6 FIB |
| `show fib mpls` | global | 显示 NetNexus MPLS FIB |
| `show fib mpls os` | global | 显示 OS 侧 MPLS FIB |
| `show fib mpls os [vrf <name>] <label>` | global | 查询指定 label |
| `show fib nexthop {ipv4|ipv6} [vrf <name>] [id <id>]` | global | 显示 FIB 下一跳对象 |

MPLS OS 查询依赖 Linux MPLS 内核模块和运行权限。
