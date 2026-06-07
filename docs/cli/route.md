# 路由 CLI 文档

Route 模块（module-id: 7）负责 RIB、静态路由、批量静态路由、协议路由接收、下一跳对象和路由订阅展示。

## 静态路由命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `route static ipv4 <prefix> <prefix-length> <nexthop>` | config | 添加 IPv4 静态路由 |
| `route static ipv4 <prefix> <prefix-length> <nexthop> metric <value>` | config | 添加带 metric 的 IPv4 静态路由 |
| `route static ipv4 <prefix> <prefix-length> <nexthop> interface <ifname>` | config | 添加指定出接口的 IPv4 静态路由 |
| `route static ipv4 <prefix> <prefix-length> interface <ifname>` | config | 添加 interface-only IPv4 静态路由 |
| `route static ipv6 <prefix> <prefix-length> <nexthop>` | config | 添加 IPv6 静态路由 |
| `route static ipv6 <prefix> <prefix-length> <nexthop> metric <value>` | config | 添加带 metric 的 IPv6 静态路由 |
| `route static ipv6 <prefix> <prefix-length> <nexthop> interface <ifname>` | config | 添加指定出接口的 IPv6 静态路由 |
| `route static ipv6 <prefix> <prefix-length> interface <ifname>` | config | 添加 interface-only IPv6 静态路由 |

以上命令的 IPv4/IPv6 interface 形式也支持 `metric <value>`。

删除命令：

| 命令 | 视图 |
| --- | --- |
| `no route static ipv4 <prefix> <prefix-length>` | config |
| `no route static ipv4 <prefix> <prefix-length> <nexthop>` | config |
| `no route static ipv4 <prefix> <prefix-length> <nexthop> interface <ifname>` | config |
| `no route static ipv4 <prefix> <prefix-length> interface <ifname>` | config |
| `no route static ipv6 <prefix> <prefix-length>` | config |
| `no route static ipv6 <prefix> <prefix-length> <nexthop>` | config |
| `no route static ipv6 <prefix> <prefix-length> <nexthop> interface <ifname>` | config |
| `no route static ipv6 <prefix> <prefix-length> interface <ifname>` | config |

## 批量静态路由命令

`route static-batch` 用于生成大量连续静态路由，支持 IPv4/IPv6、VRF、nexthop、interface-only、metric 和 count。

常用形式：

```text
route static-batch <name> ipv4 [vrf <name>] <start> <pfx4> <nexthop> [metric <value>] count <N>
route static-batch <name> ipv4 [vrf <name>] <start> <pfx4> interface <ifname> [metric <value>] count <N>
route static-batch <name> ipv6 [vrf <name>] <start> <pfx6> <nexthop> [metric <value>] count <N>
route static-batch <name> ipv6 [vrf <name>] <start> <pfx6> interface <ifname> [metric <value>] count <N>
no route static-batch <name>
```

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show route {ipv4|ipv6} [vrf <name>]` | global | 显示 RIB 路由 |
| `show route ipv4 [vrf <name>] <destination> <prefix-length>` | global | 查询指定 IPv4 前缀 |
| `show route ipv6 [vrf <name>] <destination> <prefix-length>` | global | 查询指定 IPv6 前缀 |
| `show route {ipv4|ipv6} [vrf <name>] proto {static|bgp|ospf|connected|isis}` | global | 按协议过滤 |
| `show route summary {ipv4|ipv6} [vrf <name>]` | global | 显示路由统计 |
| `show route subscribe {ipv4|ipv6} [vrf <name>]` | global | 显示路由订阅者 |
| `show route static {ipv4|ipv6} [vrf <name>]` | global | 显示候选静态路由表 |
| `show route relay {ipv4|ipv6} [vrf <name>]` | global | 显示协议模块送入 route 的 relay 路由 |
| `show route relay {ipv4|ipv6} [vrf <name>] proto {bgp|static}` | global | 按 relay 来源过滤 |
| `show route nexthop {ipv4|ipv6} [vrf <name>] [id <id>]` | global | 显示下一跳对象 |
| `show route static nexthop {ipv4|ipv6} [vrf <name>] [id <id>]` | global | 显示静态路由下一跳组 |
