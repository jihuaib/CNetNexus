# RPM CLI 文档

RPM（Routing Policy Manager，module-id: 18）集中保存路由策略，并按用途向业务模块提供查询和变更通知。当前已接入 BGP 出口策略。

## 策略与节点

```text
route-policy <name> {permit|deny} node <sequence>
no route-policy <name>
no route-policy <name> node <sequence>
```

`route-policy` 是通用策略，不声明由 BGP、OSPF 或重分发等哪个业务使用。创建或进入节点时会切换到 Route-Policy 视图，提示符为：

```text
<NetNexus(config-route-policy-<name>-node-<sequence>)>
```

节点按 `sequence` 从小到大求值，首个匹配节点结束求值。`permit` 节点允许路由并执行 `apply`，`deny` 节点拒绝路由；没有任何节点匹配时隐式拒绝。没有配置 `if-match` 的节点无条件匹配。

## 匹配条件

| 命令 | 说明 |
| --- | --- |
| `if-match network ipv4 <address> <prefix-length>` | 匹配该 IPv4 前缀及其更具体路由 |
| `if-match network ipv6 <address> <prefix-length>` | 匹配该 IPv6 前缀及其更具体路由 |
| `no if-match network` | 删除前缀匹配条件，使节点无条件匹配 |

当前一个节点只支持一个前缀条件。后续增加 community、AS path 等条件时，使用 `match_mask` 扩展，同一节点内不同匹配类型采用 AND 关系。

## 属性动作

| 命令 | 说明 |
| --- | --- |
| `apply med <0-4294967295>` | 设置 BGP MED |
| `no apply med` | 删除 MED 动作 |
| `apply local-preference <0-4294967295>` | 设置 BGP Local Preference |
| `no apply local-preference` | 删除 Local Preference 动作 |
| `apply community <asn:value>` | 设置 BGP Community |
| `no apply community` | 删除 Community 动作 |

## BGP 出口绑定

在 BGP 地址族视图配置：

```text
neighbor <ip-address> route-policy <name> export
no neighbor <ip-address> route-policy export
```

BGP 在写入配置前同步查询 RPM。策略不存在时命令失败，不会留下悬空的新引用。策略绑定后如被删除，BGP 保留配置名称但立即进入 fail-closed 状态，撤销该邻居已有的 Adj-RIB-Out，并拒绝继续发布路由；同名策略重建后自动恢复并重新计算。

## 示例

```text
route-policy TO-UPLINK permit node 10
 if-match network ipv4 10.0.0.0 8
 apply med 50
 apply community 65000:100
exit
route-policy TO-UPLINK deny node 100
exit

bgp 65000
 neighbor 192.0.2.2 as 65001
 af ipv4-unicast
  neighbor 192.0.2.2 enable
  neighbor 192.0.2.2 route-policy TO-UPLINK export
```
