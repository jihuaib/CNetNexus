# ISIS CLI 文档

ISIS 模块（module-id: 9）提供实例配置、接口使能、LAN 邻居、LSDB、SPF 和路由同步。

## 实例命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `isis <tag>` | config | 创建或进入 ISIS 实例视图 |
| `isis <tag> vrf <vrf-name>` | config | 创建或进入指定 VRF 的 ISIS 实例；`vrf` 位于 tag 之后 |
| `no isis [<tag>]` | config, isis | 删除实例；在 ISIS 视图中可省略 `<tag>` 删除当前实例 |

ISIS 视图提示符为 `<NetNexus(config-isis-{ctx:8})>`。
未带 `vrf` 的实例属于 `public`。接口所属 VRF 必须与实例绑定的 VRF 一致，IPv4 和
IPv6 路由会写入该 VRF 的 RIB、FIB 和 Linux 路由表。

## ISIS 视图命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `net <system-id>` | isis | 设置 NET/System-ID |
| `is-type {level-1|level-2|level-1-2}` | isis | 设置实例 level 类型 |
| `cost-style {narrow|wide}` | isis | 设置 metric 风格 |
| `af ipv4` | isis | 启用 IPv4 AF |
| `af ipv6` | isis | 启用 IPv6 AF，并进入 ISIS IPv6 AF 子视图 |
| `no af ipv4` | isis | 关闭 IPv4 AF |
| `no af ipv6` | isis | 关闭 IPv6 AF |

IPv6 AF 子视图提示符为 `<NetNexus(config-isis-{tag}-af-ipv6)>`。SRv6 locator
默认不发布，必须在该子视图显式选择：

```text
isis 160
 cost-style wide
 af ipv6
  segment-routing srv6 locator loc-r1-be
```

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `segment-routing srv6 locator <locator-name>` | isis-af-ipv6 | 将指定的本地 locator 前缀发布到 ISIS IPv6；只发布所选 locator |
| `no segment-routing srv6` | isis-af-ipv6 | 停止发布 locator，并触发 LSP 更新与远端路由撤销 |

locator 必须已在 SRv6 模块中创建。当前只允许 public VRF、已启用 IPv6 AF 且
`cost-style wide` 的 ISIS 实例配置该功能；配置 locator 发布期间不能关闭 IPv6 AF。

## 接口视图命令

ISIS 接口命令在 `if` 和 `if-loop` 视图可用，IPv4 与 IPv6 使用不同命令前缀。

| IPv4 命令 | IPv6 命令 | 说明 |
| --- | --- | --- |
| `isis enable <tag>` | `isis ipv6 enable <tag>` | 在接口启用 ISIS |
| `no isis enable <tag>` | `no isis ipv6 enable <tag>` | 在接口关闭 ISIS |
| `isis metric <tag> <metric>` | `isis ipv6 metric <tag> <metric>` | 设置接口 metric，范围 1-16777215 |
| `no isis metric <tag>` | `no isis ipv6 metric <tag>` | 恢复接口 metric |
| `isis hello-interval <tag> <seconds>` | `isis ipv6 hello-interval <tag> <seconds>` | 设置 hello 间隔 |
| `no isis hello-interval <tag>` | `no isis ipv6 hello-interval <tag>` | 恢复 hello 间隔 |
| `isis hold-multiplier <tag> <value>` | `isis ipv6 hold-multiplier <tag> <value>` | 设置 hold multiplier，范围 1-100 |
| `no isis hold-multiplier <tag>` | `no isis ipv6 hold-multiplier <tag>` | 恢复 hold multiplier |
| `isis passive <tag>` | `isis ipv6 passive <tag>` | 设置 passive |
| `no isis passive <tag>` | `no isis ipv6 passive <tag>` | 取消 passive |

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show isis summary ipv4 <tag>` | global | 显示 IPv4 AF 实例概要 |
| `show isis summary ipv6 <tag>` | global | 显示 IPv6 AF 实例概要 |
| `show isis interface ipv4 <tag>` | global | 显示 IPv4 ISIS 接口状态 |
| `show isis interface ipv6 <tag>` | global | 显示 IPv6 ISIS 接口状态 |
| `show isis neighbor <tag>` | global | 显示邻居 |
| `show isis neighbor <tag> verbose` | global | 显示邻居详细信息 |
| `show isis lsdb ipv4 <tag>` | global | 显示 IPv4 LSDB |
| `show isis lsdb ipv6 <tag>` | global | 显示 IPv6 LSDB |
| `show isis route ipv4 <tag>` | global | 显示 IPv4 ISIS 路由 |
| `show isis route ipv6 <tag>` | global | 显示 IPv6 ISIS 路由 |
| `show isis route ipv4 <tag> <destination> <mask>` | global | 查询 IPv4 ISIS 前缀 |
| `show isis route ipv6 <tag> <destination> <mask>` | global | 查询 IPv6 ISIS 前缀 |

ISIS 当前实现包括 LAN DIS/pseudonode、邻居地址学习、SPF 多路径和到 Route 模块的路由同步。
