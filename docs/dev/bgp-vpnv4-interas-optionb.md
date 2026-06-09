# VPNv4 Inter-AS Option B 端到端流程

本文档描述 NetNexus 实现的 **VPNv4 跨域 Option B**(ASBR 无 VRF、域间 eBGP vpnv4 直连、每跳换标)
的控制面与转发面完整流程,逐台设备拆解。

相关文档:[VRF 路由导出](bgp-vpnv4-vrf-export.md)、[VRF 路由导入](bgp-vpnv4-vrf-import.md)、[TUNNEL CLI](../cli/tunnel.md)。
对照实现:[Option A](#与-option-a-的差异) 见 `scripts/ci/modules/bgp/n4-l3-g12/vpnv4_optiona_interas.py`,
本方案 CI 见 `scripts/ci/modules/bgp/n4-l3-g12/vpnv4_optionb_interas.py`。

---

## 1. 拓扑与角色

```
        AS 65001                                AS 65002
┌──────────────────────┐                ┌──────────────────────┐
│  r1(PE1) ─── r2(ASBR1) ════ eBGP ════ r3(ASBR2) ─── r4(PE4)  │
│           10.12.0.0/30   vpnv4直连   10.34.0.0/30            │
└──────────────────────┘  10.23.0.0/30 └──────────────────────┘
   ISIS + LDP + iBGP vpnv4                ISIS + LDP + iBGP vpnv4
```

| 设备 | 角色 | VRF | 域间链路 |
|------|------|-----|----------|
| r1 | PE1(入口/出口 PE) | VRF red(CE 侧) | — |
| r2 | ASBR1 | **无 VRF** | r2-GE2 ↔ r3-GE1,eBGP 65001↔65002 |
| r3 | ASBR2 | **无 VRF** | r3-GE1 ↔ r2-GE2,eBGP 65002↔65001 |
| r4 | PE4(入口/出口 PE) | VRF red(CE 侧) | — |

- 域内:ISIS 撑 IGP、LDP 撑传输 LSP、iBGP vpnv4(r1↔r2、r3↔r4)。
- 域间:r2↔r3 **直连 eBGP vpnv4**,两个 ASBR **都不配 VRF**。
- Option B 特征:VPN 路由在 ASBR 之间用 vpnv4 直接传递,ASBR 不解私网、**每跳换标(SWAP)**。

本文以目的前缀 `100.4.4.4/32`(r4 的 VRF red,例如本端环回)为例,描述 **r1 → r4** 方向。

---

## 2. 三块核心改动(支撑控制面)

| # | 改动 | 作用点 | 文件 |
|---|------|--------|------|
| 1 | `no policy vpn-target` 接受并中转 vpnv4 | ASBR 无 import-RT,默认会丢 vpnv4;关闭 RT 入向过滤 | `bgp_adj_rib_in.c`,实例 flag `BGP_INST_FLAG_VPN_TARGET_FILTER` |
| 2 | VPN 族 eBGP→iBGP 默认改下一跳(next-hop-self) | 让本域 PE 能解析下一跳,否则路由恒 Unresolved | `bgp_update_group.c:305 bgp_select_nh_rule()` |
| 3 | ASBR 中转换标(申请本地标签 + SWAP ILM) | 改下一跳后必须自己换标,装数据面 SWAP | `bgp_vrf_import.c` + `tunnel_rib.c` |

> 改动 2 是触发点:**只要 ASBR 改了下一跳,就必须自己换标(改动 3)**,否则它把流量吸引到自己却无法转发。

---

## 3. 控制面:路由传播逐跳分析(r4 → r1)

VPN 路由从 r4 起源,逆向逐跳传到 r1。

### r4(PE4)— 路由起源 + 分配 VPN 标签

1. CE 侧 / 本地配置在 **VRF red** 学到 `100.4.4.4/32`。
2. VRF 导出子系统按 export-RT 把私网路由灌入 vpnv4 loc-RIB(见 [VRF 路由导出](bgp-vpnv4-vrf-export.md))。
3. 发送时按 per-vrf 申请本地 VPN 标签 `Lr4`,注入 NLRI(`bgp_update_group.c:1432` 本地起源分支)。
4. 经 iBGP vpnv4 通告给 r3:`NLRI=100.4.4.4/32, label=Lr4, nexthop=r4-loopback, RT=red`。

### r3(ASBR2)— 接受中转 + 改下一跳 + 换标

1. **接受**:r3 无 VRF/无 import-RT,靠 `no policy vpn-target` 关闭 RT 过滤,把这条 vpnv4 收进 loc-RIB
   (`best->has_label=true, best->label=Lr4`,下一跳=r4-loopback)。
2. **改下一跳**:向 r2 发布(eBGP,`sess_type=EBGP`)→ `bgp_select_nh_rule()` 返回 `BGP_NH_RULE_LOCAL`,
   下一跳改成 r3-loopback。
3. **换标**:发送注入分支(`bgp_update_group.c:1445`,条件 = vpnv4 + `rule==LOCAL` + `best->has_label`)调
   `bgp_vrf_import_transit_alloc_label(best)`:
   - 申请本地入标签 `Lr3`,记录 `best->out_local_label=Lr3` / `transit_owner_id`;
   - 向 TUNNEL 下 SWAP binding:`{action=SWAP, swap_label=Lr4, endpoint=r4-loopback}`
     (endpoint 取**改下一跳之前的原始 BGP 下一跳**,`bgp_vrf_import.c:291`);
   - 用 `Lr3` 替换 NLRI 标签。
4. 向 r2 通告:`NLRI=100.4.4.4/32, label=Lr3, nexthop=r3-loopback`。

### r2(ASBR1)— 接受中转 + 改下一跳 + 换标(直连)

1. **接受**:同样靠 `no policy vpn-target` 收下(`best->label=Lr3`,下一跳=r3-loopback,来源 eBGP)。
2. **改下一跳**:向 r1 发布(iBGP)→ src_class=`FROM_EBGP` 且是 VPN 族 → `bgp_select_nh_rule()` 返回
   `LOCAL`(`bgp_update_group.c:327`),下一跳改成 r2-loopback。
3. **换标**:申请本地入标签 `Lr2`;SWAP binding `{swap_label=Lr3, endpoint=r3-loopback}`。
   注意 endpoint=r3 是**直连**(eBGP 对端),不经域内 LDP。
4. 向 r1 通告:`NLRI=100.4.4.4/32, label=Lr2, nexthop=r2-loopback`。

### r1(PE1)— 导入 VRF + 解析隧道

1. 收到 vpnv4 路由,按 VRF red 的 import-RT 命中 → 导入 VRF red(见 [VRF 路由导入](bgp-vpnv4-vrf-import.md))。
2. 下一跳=r2-loopback,在**本域可经 LDP 解析**(改动 2 的价值);Out-Label=`Lr2`。
3. 私网 `100.4.4.4/32` 在 VRF red 可达,FIB installed。

> **控制面验证证据**:r1 上 VRF red `100.4.4.4` 的 `Out-Label = Lr2`(r2 的本地标签,**非透传 r4 的标签**),
> 这是 Option B 区别于 Option C 的关键 —— 标签逐跳重写。

---

## 4. 转发面:每台设备的 MPLS 表项与标签栈

### 4.1 标签链总览(r1 → r4 找 100.4.4.4)

| 节点 | 角色 | 转发动作 |
|------|------|----------|
| **r1**(PE1) | 入口 | VRF 查表 → push `[LDP到r2 的传输标签, Lr2]`,发往 r2 |
| **r2**(ASBR1) | 换标 | ILM:`Lr2` → SWAP 成 `Lr3`,直连发 r3(BGP_ADJ,**无外层传输标签**) |
| **r3**(ASBR2) | 换标 | ILM:`Lr3` → SWAP 成 `Lr4` + push `[LDP到r4 的传输标签]`,发往 r4 |
| **r4**(PE4) | 出口 | ILM:`Lr4` → POP → 进 VRF red 查表转给 CE |

私网报文始终封在 VPN 标签里,ASBR 全程不解私网。每个 ASBR 各换一次标。

### 4.2 各设备转发表项

**r1(PE1,入口)**
- VRF red FTN/NHLFE:`100.4.4.4/32` → out 标签栈 = `[LDP_to_r2, Lr2]`,出口 = 朝 r2 的接口。
- 标签栈含两层:外层 LDP 传输标签(送到 r2),内层 `Lr2`(VPN 标签,r2 据此换标)。

**r2(ASBR1,直连换标)**
- **一条 SWAP ILM**:`in=Lr2 → SWAP swap_label=Lr3`,出口由 `endpoint=r3` 解析。
- 解析命中 `bgp_relay.c:1491 bgp_relay_session_lu_adj_sync()` 注册的 **BGP_ADJ 直连假隧道**
  (`source_type=TUNNEL_SOURCE_BGP_ADJ`,`label_count=0`,`out_ifindex`=直连口)。
- 出栈只有 `[Lr3]`,**无外层传输标签**(直连一跳,不需要 LSP)。

**r3(ASBR2,经 LDP 换标)**
- **一条 SWAP ILM**:`in=Lr3 → SWAP swap_label=Lr4`,出口由 `endpoint=r4-loopback` 解析。
- 解析命中 **LDP 传输 LSP**(`source_type=TUNNEL_SOURCE_LDP`),把 LDP 传输标签叠进出栈。
- 出栈 = `[LDP_to_r4, Lr4]`:外层 LDP 传输标签 + 最内层 `Lr4`(BoS)。
- r3 无 VRF,**不建 POP 进 VRF 表项**,只此一条 SWAP ILM。

**r4(PE4,出口)**
- ILM:`in=Lr4 → POP`,弹出 VPN 标签后进 **VRF red** 查表,转给 CE。

### 4.3 外层 LDP 标签如何进入 SWAP ILM(关键机制)

ASBR 申请标签时,**只产生一条 SWAP ILM**;外层 LDP 传输标签**不是 ILM 主动"发给"LDP**,而是
TUNNEL 在生成 ILM 那一刻,从 LDP 预先注册的隧道候选里**迭代取出、叠进出栈**。LDP 与 BGP 在 TUNNEL 里解耦:

1. **LDP 侧(先发生)**:LDP 把"去某 loopback 的传输 LSP"作为 **candidate** 注册进 `rib->candidates`,
   `source_type=TUNNEL_SOURCE_LDP`,`candidate->labels[]` 携带传输标签。
2. **BGP 侧(收到路由时)**:BGP 申请本地标签后,只向 TUNNEL 下一条 `action=SWAP` 的 **label binding**
   (`swap_label` + `endpoint`),**不碰 LDP**。
3. **TUNNEL recompute 时缝合**(`tunnel_rib.c:703 tunnel_append_transit_swap_ilms()`):
   - 用 binding 的 `endpoint` 调 `tunnel_resolve()`(`tunnel_rib.c:614`);
   - `tunnel_resolve_inner()` 在 candidates 里按 endpoint 选最优候选(`tunnel_candidate_best()`);
     命中 LDP 则迭代其传输栈,命中 BGP_ADJ 则 `label_count=0`;
   - `tunnel_stack_append()` 把候选的传输标签拷进 `notify->labels`,带出 `out_ifindex`/`relay_addr`;
   - 回到 `tunnel_rib.c:750`,组最终出栈:**传输标签在外层 + `swap_label` 在最内层(BoS)**,装成 ILM。

> 同一段代码,候选是 LDP 就有外层传输标签(r3),候选是 BGP_ADJ 就没有(r2)—— 逻辑统一。
> 该 SWAP ILM 为 `nhlfe_id=0` 的"自带出口"型(`tunnel_rib.c:745`),出口信息直接挂 ILM 上;
> `tunnel_append_local_pop_ilms()` 显式跳过 SWAP 绑定,避免冲突。

### 4.4 下发 FIB

- `tunnel_fill_fib_ilm` 对 `SWAP && nhlfe_id==0` 直接用 ILM 自带的 `out_ifindex`/`relay_addr`/`labels[]` 下 FIB。
- `fib_os_mpls_route_send` 的 `TUNNEL_ACTION_SWAP` 分支(RTA_NEWDST 标签栈 + OIF + via)无改动即支持。

---

## 5. 标签释放

- 中转标签为 **per-route**,记录在 `bgp_route_node_t.out_local_label` / `transit_owner_id`(`bgp_rib.h:81`)。
- 节点回收时(`bgp_rib.c::route_node_release_attrs`)调 `bgp_vrf_import_transit_release_label()`
  (`bgp_vrf_import.c:318`),向 TUNNEL 发 release,撤掉 SWAP ILM。
- best 切换时是不同节点各自持标签,旧节点回收时释放;未做 RecvLabel 就地更新,语义正确。

---

## 6. 与 Option A 的差异

| 维度 | Option A | Option B(本文) |
|------|----------|------------------|
| ASBR 是否建 VRF | 每个 VPN 建 VRF | **无 VRF** |
| 域间承载 | per-VRF 子接口跑普通 IPv4 | **vpnv4 直接对接** |
| 数据面 | 纯 IP 转发,无标签穿越 | **每跳 SWAP 换标** |
| 控制面 | VRF-to-VRF | 接受中转 + 改下一跳 + 换标 |
| CI | `vpnv4_optiona_interas.py` | `vpnv4_optionb_interas.py` |

---

## 7. 当前限制

- 仅支持 **vpnv4**(vpnv6 的 release 路径硬编码 `afi=IPV4`,`bgp_vrf_import.c:327`)。
- 中转标签 per-route,未做 best 切换时 RecvLabel 变更的就地更新(best 切换=不同节点,各自标签,旧节点回收时释放)。

---

## 8. 关键代码索引

| 功能 | 文件:行 |
|------|---------|
| 改下一跳规则 | `src/bgp/work/bgp_update_group.c:305 bgp_select_nh_rule()` |
| 发送注入(本地起源 / 中转) | `src/bgp/work/bgp_update_group.c:1432 / :1445` |
| 中转标签申请 / 释放 | `src/bgp/work/bgp_vrf_import.c:276 / :318` |
| 中转节点字段 | `src/bgp/work/bgp_rib.h:81-82(out_local_label/transit_owner_id)` |
| SWAP ILM 生成 | `src/tunnel/work/tunnel_rib.c:703 tunnel_append_transit_swap_ilms()` |
| 隧道解析(取传输标签) | `src/tunnel/work/tunnel_rib.c:614 tunnel_resolve()` / `:574 _inner` |
| eBGP 直连假隧道注册 | `src/bgp/work/bgp_relay.c:1491 bgp_relay_session_lu_adj_sync()` |
| RT 入向过滤门控 | `src/bgp/work/bgp_adj_rib_in.c`(`BGP_INST_FLAG_VPN_TARGET_FILTER`) |
