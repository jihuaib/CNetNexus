# 接口 CLI 文档

IF 模块（module-id: 5）负责物理接口、loopback、null0、接口地址、管理状态和 VRF 绑定。

## 接口进入命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `if GE-1` ... `if GE-8` | config | 进入 GE 接口视图 |
| `if null0` | config | 进入 null0 接口视图 |
| `if loop <loop-id>` | config | 创建或进入 loopback 接口视图，`<loop-id>` 范围 1-1024 |
| `no if loop <loop-id>` | config | 删除 loopback 接口 |

GE 视图提示符为 `<NetNexus(config-if-GE-{ctx:4})>`，loopback 视图提示符为 `<NetNexus(config-if-loop{ctx:6})>`。

## 接口配置命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ip address <ip-address> <prefix-len>` | if, if-loop | 配置 IPv4 地址 |
| `no ip address <ip-address> <prefix-len>` | if, if-loop | 删除指定 IPv4 地址 |
| `ipv6 address <ipv6-address> <prefix-len>` | if, if-loop | 配置 IPv6 地址 |
| `no ipv6 address <ipv6-address> <prefix-len>` | if, if-loop | 删除指定 IPv6 地址 |
| `shutdown` | if | 管理关闭 GE 接口 |
| `no shutdown` | if | 管理开启 GE 接口 |
| `vrf forwarding <vrf-name>` | if, if-loop | 将接口绑定到 VRF，执行时会清空该接口 IP 地址 |
| `no vrf forwarding` | if, if-loop | 解绑 VRF，执行时会清空该接口 IP 地址 |

IPv4 和 IPv6 地址独立保存，同一接口可以同时配置两种地址族。

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show if` | global | 显示所有接口概要 |
| `show if GE-1` ... `show if GE-8` | global | 显示指定 GE 接口详情 |
| `show if loop <loop-id>` | global | 显示指定 loopback 接口详情 |
| `show if subscribe` | global | 显示 IF 事件订阅者 |
