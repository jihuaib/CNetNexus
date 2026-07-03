# VRF CLI 文档

VRF 模块（module-id: 4）负责 VRF 实例、地址族、RD/RT、label 分配策略和 OS 侧 VRF 状态展示。

## 实例命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `vrf <vrf-name>` | config | 创建或进入 VRF 视图 |
| `no vrf <vrf-name>` | config | 删除 VRF 实例 |

VRF 视图提示符为 `<NetNexus(config-vrf-{ctx:5})>`。

## 地址族命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `af ipv4` | vrf | 进入 IPv4 地址族视图 |
| `af ipv6` | vrf | 进入 IPv6 地址族视图 |
| `no af {ipv4|ipv6}` | vrf | 删除对应地址族配置 |

地址族视图提示符为 `<NetNexus(config-vrf-<name>-af-ipv4)>` 或 `<NetNexus(config-vrf-<name>-af-ipv6)>`。

## 地址族配置

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `route-distinguisher <rd>` | vrf-af | 配置 RD，格式如 `65000:1` 或 `1.1.1.1:1` |
| `no route-distinguisher` | vrf-af | 删除 RD |
| `vpn-target <rt>` | vrf-af | 配置 import/export 均生效的 RT |
| `vpn-target <rt> {import|export|both}` | vrf-af | 配置指定方向 RT |
| `no vpn-target <rt>` | vrf-af | 删除 RT |
| `no vpn-target <rt> {import|export|both}` | vrf-af | 删除指定方向 RT |
| `apply-label {per-vrf|per-route}` | vrf-af | 设置导出到 VPNv4 时的 label 分配策略 |
| `no apply-label` | vrf-af | 恢复默认 `per-vrf` |

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show vrf` | global | 显示所有 VRF |
| `show vrf name <vrf-name>` | global | 显示指定 VRF 详情 |
| `show vrf subscribe` | global | 显示 VRF 事件订阅者 |
| `show vrf os` | global | 通过 netlink 显示 OS 侧 L3VRF 设备 |
