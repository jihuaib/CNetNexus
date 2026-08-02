# BGP CLI 文档

BGP 模块（module-id: 6）提供全局 BGP、VRF BGP、多个地址族、邻居、路由导入、路由反射、Route Refresh、QP 路由和 BMP 采集器配置。

## 实例命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `bgp <as-number>` | config | 创建或进入全局 BGP 视图 |
| `no bgp [as-number]` | config | 删除 BGP 配置 |
| `vrf <vrf-name>` | bgp | 进入 BGP VRF 视图 |

全局 BGP 视图提示符为 `<NetNexus(config-bgp)>`，VRF BGP 视图提示符为 `<NetNexus(config-bgp-vrf-<name>)>`。

## BGP 视图命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `router-id <ip-address>` | bgp, bgp-vrf | 设置 Router ID |
| `no router-id` | bgp, bgp-vrf | 恢复默认 Router ID |
| `timer connect-retry <seconds>` | bgp, bgp-vrf | 设置连接重试时间 |
| `no timer connect-retry` | bgp, bgp-vrf | 恢复连接重试默认值 |
| `timer keepalive <keepalive-time> hold <hold-time>` | bgp, bgp-vrf | 设置 keepalive/hold timer |
| `no timer keepalive` | bgp, bgp-vrf | 恢复 keepalive/hold 默认值 |
| `neighbor <ipv4|ipv6> as <as-num>` | bgp, bgp-vrf | 配置邻居和远端 AS |
| `no neighbor <ipv4|ipv6>` | bgp, bgp-vrf | 删除邻居 |
| `neighbor <ipv4|ipv6> open-capability {as4|route-refresh}` | bgp, bgp-vrf | 打开邻居能力 |
| `no neighbor <ipv4|ipv6> open-capability {as4|route-refresh}` | bgp, bgp-vrf | 关闭邻居能力 |
| `neighbor <ipv4|ipv6> source-interface <if-name>` | bgp, bgp-vrf | 指定邻居源接口 |
| `no neighbor <ipv4|ipv6> source-interface` | bgp, bgp-vrf | 删除邻居源接口 |
| `neighbor <ipv4|ipv6> ebgp-multihop <ttl>` | bgp, bgp-vrf | 配置 EBGP multihop TTL |
| `no neighbor <ipv4|ipv6> ebgp-multihop` | bgp, bgp-vrf | 删除 EBGP multihop |

## 地址族命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `af ipv4-unicast` | bgp, bgp-vrf | 进入 IPv4 unicast AF |
| `af ipv6-unicast` | bgp, bgp-vrf | 进入 IPv6 unicast AF |
| `af vpnv4` | bgp | 进入全局 VPNv4 AF |
| `af vpnv6` | bgp | 进入全局 VPNv6 AF |
| `af evpn` | bgp | 进入全局 EVPN AF |
| `af ipv4-qp` | bgp | 进入 IPv4 QP AF |
| `af ipv6-qp` | bgp | 进入 IPv6 QP AF |
| `af ipv4-labeled` | bgp | 进入 IPv4 labeled-unicast AF |
| `no af {ipv4-unicast|ipv6-unicast|ipv4-qp|ipv6-qp|ipv4-labeled|vpnv4|vpnv6|evpn}` | bgp | 删除全局 AF |
| `no af {ipv4-unicast|ipv6-unicast}` | bgp-vrf | 删除 VRF AF |

## 地址族视图命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `neighbor <ipv4|ipv6> enable` | bgp-af, bgp-vrf-af | 在当前 AF 激活邻居 |
| `no neighbor <ipv4|ipv6> enable` | bgp-af, bgp-vrf-af | 在当前 AF 取消激活邻居 |
| `import-route static` | bgp-af, bgp-vrf-af | 导入静态路由 |
| `no import-route static` | bgp-af, bgp-vrf-af | 停止导入静态路由 |
| `import-route connected` | bgp-af, bgp-vrf-af | 导入直连路由 |
| `no import-route connected` | bgp-af, bgp-vrf-af | 停止导入直连路由 |
| `reflector cluster-id <ipv4>` | bgp-af, bgp-vrf-af | 配置当前 AF 的 route-reflector cluster-id |
| `no reflector cluster-id` | bgp-af, bgp-vrf-af | 删除 cluster-id |
| `neighbor <ip> reflect-client` | bgp-af, bgp-vrf-af | 将邻居设为 RR client |
| `no neighbor <ip> reflect-client` | bgp-af, bgp-vrf-af | 取消 RR client |
| `neighbor <ip> route-policy <name> export` | bgp-af, bgp-vrf-af | 将已存在的通用 RPM 策略用于邻居出口 |
| `no neighbor <ip> route-policy export` | bgp-af, bgp-vrf-af | 删除邻居出口策略绑定 |
| `segment-routing srv6 locator <locator-name>` | bgp-vrf-af-ipv4/ipv6-unicast | 为该私网 AF 选择 locator，并申请、安装一个 End.DT4/End.DT6 LocalSID；本命令不决定向哪些邻居发布 SID |
| `no segment-routing srv6` | bgp-vrf-af-ipv4/ipv6-unicast | 撤销该私网 AF 的 VPN 导出、释放 LocalSID 后重建导出；已配置 SID 的邻居对该源 AF 回退到 MPLS 标签 |
| `neighbor <ip> srv6-sid` | bgp-af-vpnv4/vpnv6 | 仅向该 VPN AF 邻居发布本地 SRv6 L3 Service Prefix-SID；默认不配置，仍发布 MPLS VPN 标签 |
| `no neighbor <ip> srv6-sid` | bgp-af-vpnv4/vpnv6 | 定向撤销该邻居的 SID 编码，重新归入 MPLS update-group 并补发 |
| `srv6 be` | bgp-vrf-af-ipv4/ipv6-unicast | 对该私网 AF 导入的合法 Service SID 执行公网 IPv6 最长匹配迭代；默认不配置，使用 MPLS 隧道迭代 |
| `no srv6 be` | bgp-vrf-af-ipv4/ipv6-unicast | 关闭 SRv6 BE 迭代，恢复默认隧道迭代 |

`import-rib public ipv4-labeled-unicast` / `no import-rib public ipv4-labeled-unicast` 在 IPv4 unicast AF 视图下可用，用于从 public IPv4 labeled-unicast 导入。

出口策略由 RPM 模块集中配置，详见 [RPM CLI](rpm.md)。RPM 不记录策略的业务用途；BGP 配置时同步校验策略名称是否存在，不存在的策略不会写入业务配置。

SRv6 L3VPN 的资源、发送和接收控制彼此独立：私网 AF 的 locator 拥有一个稳定的
End.DT4/End.DT6 SID；公网 VPN AF 的 `neighbor <ip> srv6-sid` 按邻居选择发送编码；私网 AF 的
`srv6 be` 只选择接收侧的下一跳迭代方式。共享 VPN Loc-RIB 不携带本地产生的 Prefix-SID，发送时
SID 邻居与默认邻居进入不同 update-group、subgroup、Adj-RIB-Out 和打包队列。

默认邻居的 VPN NLRI 携带真实 MPLS 服务标签。SID 邻居对已配置 locator 的源 VRF/AF 携带
Prefix-SID，whole-SID 模式的 NLRI label 为 Implicit NULL(3)；若某个源 AF 未配置 locator，
该源路由仍回退到 MPLS 标签。VPNv4 SID 更新还要求与邻居协商 RFC 8950 IPv6 nexthop 能力。
收到的 SID 路由不能被无损转换为 MPLS，因此不会发布到默认非 SID update-group。

接收侧默认仍按 MPLS 隧道迭代。只有在目标私网 AF 配置 `srv6 be` 后，合法 Service SID 才按
公网 IPv6 locator 路由进行 BE 迭代；未开启时，SID-only + label 3 的路径 fail closed，不会被
误下发为无法完成 VRF demux 的隧道路由。

QP AF 还支持：

```text
route start-dqpn <dqpn> ip <ipv4> mask <mask> count <count> bid <ipv6>
route start-dqpn <dqpn> ipv6 <ipv6> mask <mask> count <count> bid <ipv6>
route-select enable
no route-select enable
```

## Route Refresh 命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `refresh bgp neighbor <ipv4-address> {import|export} af {ipv4-unicast|ipv6-unicast} [vrf <vrf-name>]` | global | 对指定 IPv4 邻居发起 route refresh |
| `refresh bgp neighbor <ipv6-address> {import|export} af {ipv4-unicast|ipv6-unicast} [vrf <vrf-name>]` | global | 对指定 IPv6 邻居发起 route refresh |
| `refresh bgp af {ipv4-unicast|ipv6-unicast} [vrf <vrf-name>] {import|export}` | global | 对 AF 批量发起 route refresh |

## BMP 采集器命令

这些命令在 BGP 模块内配置向外部 BMP 采集器上报。

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `bmp instance <name>` | bgp | 创建或进入 BMP 实例视图 |
| `no bmp instance <name>` | bgp | 删除 BMP 实例 |
| `collector <ip> port <port>` | bgp-bmp | 配置采集器地址和端口 |
| `no collector` | bgp-bmp | 删除采集器 |
| `stats-report interval <seconds>` | bgp-bmp | 配置统计上报周期 |
| `no stats-report interval` | bgp-bmp | 恢复统计上报默认值 |
| `reconnect interval <seconds>` | bgp-bmp | 配置重连周期 |
| `no reconnect interval` | bgp-bmp | 恢复重连默认值 |
| `monitor neighbor all` | bgp-bmp | 监控所有邻居 |
| `monitor neighbor <ip>` | bgp-bmp | 监控指定邻居 |
| `no monitor neighbor <ip>` | bgp-bmp | 删除指定邻居监控 |
| `show bgp bmp [instance <name>]` | global | 显示 BMP 采集器状态 |

## 查看命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `show bgp neighbor af vpnv4 [<ip-address>]` | global | 显示 VPNv4 邻居 |
| `show bgp neighbor af vpnv6 [<ip-address>]` | global | 显示 VPNv6 邻居 |
| `show bgp neighbor af evpn [<ip-address>]` | global | 显示 EVPN 邻居 |
| `show bgp neighbor af ipv4-unicast [vrf <vrf-name>] [<ip-address>]` | global | 显示 IPv4 unicast 邻居 |
| `show bgp neighbor af ipv6-unicast [vrf <vrf-name>] [<ip-address>]` | global | 显示 IPv6 unicast 邻居 |
| `show bgp neighbor af ipv4-qp [<ip-address>]` | global | 显示 IPv4 QP 邻居 |
| `show bgp neighbor af ipv6-qp [<ip-address>]` | global | 显示 IPv6 QP 邻居 |
| `show bgp neighbor af ipv4-labeled [<ip-address>]` | global | 显示 IPv4 labeled 邻居 |
| `show bgp route af <af> ...` | global | 显示指定 AF 路由，支持前缀、VRF、RD、QP/EVPN key 或 peer Adj-RIB-In/Out 过滤 |
| `show bgp route af <af> [vrf <vrf-name>] peer <ipv4-address\|ipv6-address> recieve-routes [<ip-address> <masklen>]` | global | 显示指定 peer 的 Adj-RIB-In；同时兼容 `receive-routes` 拼写 |
| `show bgp route af {ipv4-qp\|ipv6-qp} peer <ipv4-address\|ipv6-address> recieve-routes [<qp-route-key>]` | global | 显示指定 peer 的 QP Adj-RIB-In，可按 `dqpn=<n>,ip=<prefix>/<mask>` 或 `dqpn=<n>,ipv6=<prefix>/<mask>` 过滤 |
| `show bgp route af evpn [rd <rd>] [<evpn-route-key>]` | global | 显示 EVPN 路由；Type-5 可按展示中的 `evpn:type=5,rd=<rd>,ethag=<n>,prefix=<prefix>/<mask>` 查询，ESI/GW/Label 在详情中显示 |
| `show bgp route af <af> [vrf <vrf-name>] peer <ipv4-address\|ipv6-address> advertise-routes [<ip-address> <masklen>]` | global | 显示指定 peer 所在打包组发出的 Adj-RIB-Out 路由 |
| `show bgp route af {ipv4-qp\|ipv6-qp} peer <ipv4-address\|ipv6-address> advertise-routes [<qp-route-key>]` | global | 显示指定 peer 所在打包组发出的 QP Adj-RIB-Out 路由 |
| `show bgp attr af <af> ...` | global | 显示路径属性表 |
| `show bgp update-group af <af> ...` | global | 显示 update-group |

`<af>` 当前包括 `vpnv4`、`vpnv6`、`evpn`、`ipv4-unicast`、`ipv6-unicast`、`ipv4-qp`、`ipv6-qp`、`ipv4-labeled`。
