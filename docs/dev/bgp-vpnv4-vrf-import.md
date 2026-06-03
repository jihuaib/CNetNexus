# BGP vpnv4 路由按 import-RT 导入私网 VRF

> 与 [bgp-vpnv4-vrf-export.md](bgp-vpnv4-vrf-export.md) 方向相反、相互对称。
> 实现：`src/bgp/work/bgp_vrf_import.{c,h}`。

## 需求

- BGP 维护全量 import-RT（IRT）数据。
- 收到 vpnv4 路由时：
  - 若该路由携带的 RT **不命中**任何私网 VRF 的 import-RT → **整条丢弃**（不入公网 vpnv4 RIB，也不导入任何 VRF）。
  - 若**命中** → 保留 vpnv4 副本（公网 vpnv4 RIB），并把私网前缀（剥 RD 成 unicast）导入**每个命中的** VRF 的 ipv4-unicast RIB。

## 设计

### 1. IRT 索引（`bgp_vrf_import.c` 模块级单例）

```
g_bgp_irt_index : GHashTable<规范化 8B RT → inner GHashTable<vrf_id → refcount>>
```

- 只索引 **ipv4-unicast** 方向的 import RT（vpnv4 → ipv4 VRF），由 `bgp_apply_vrf.c`
  在 `VRF_EVENT_AF_IMPORT_RT_ADD/DEL` 时按 `afi==IPV4 && safi==UNICAST` 增量维护。
- RT 规范化复用 `bgp_ext_community_rt_canon()`（与 export 合 RT 同一套规范化），使配置 RT
  与线上 ext-community RT 8 字节直接可比。
- 生命周期：`VRF_DEL` → `purge_vrf`；`AF_DISABLE`(ipv4-uc) → `purge_vrf`；VRF 进程重启
  (`BGP_CMD_TYPE_VRF_DOWN`) → `purge_all`，随后由 REPLAY 的 `IMPORT_RT_ADD` 重建。
- **全部在 BGP worker 线程内访问**（VRF 事件经 `bgp_cmd` 投递到 worker 处理），无需加锁。

### 2. 入向过滤（`bgp_relay_ingest_peer_update`）

收到 vpnv4 reach NLRI 时，用本 UPDATE 的属性里的 RT 调 `bgp_vrf_import_attr_has_match()`
（一次 UPDATE 内 RT 属性共享，仅算一次）：

- 无匹配 → `continue` 丢弃，不入公网 vpnv4 RIB（不计 injected/failed）。
- 有匹配 → 正常 upsert 进公网 vpnv4 RIB；导入由下一步的 reconcile 完成。

### 3. 导入 reconcile

`reconcile_head(public-vpnv4-inst, head)` 对该 vpnv4 head：

1. `pick_import_source`：取来源路径——优先 VALID best，其次 route_list 中首个**非**本地导出合成路径
   （`BGP_ROUTE_FLAG_IMPORT`，避免把自己导出的路由再导入成环）。
2. 由来源路径的 RT 算出命中的私网 vrf_id 集合。
3. 遍历所有私网 VRF：
   - 命中 → 把来源（剥 RD 成 unicast NLRI）作为合成导入路径写入该 VRF 的 ipv4-unicast RIB。
   - 不命中（或来源消失） → 撤销该 (rd,prefix) 在该 VRF 的合成导入路径。

reconcile 的两条触发路径：
- **calc-done**：`bgp_vrf_import_on_calc_done` 挂在 `bgp_calc_route_select` 三处末尾（与
  `bgp_vrf_export_on_calc_done` 并列），覆盖 best 变化（valid 场景）。
- **ingest 显式触发**：`bgp_vrf_import_on_vpn_received` 由 `bgp_relay` 在 vpnv4 reach 入库成功后调用，
  把该 NLRI 推入 vpnv4 calc 队列。**这是必需的**：L3VPN 的 vpnv4 下一跳是远端 PE，无 LSP 时收到的
  路由恒为 invalid，不产生 best 变化、不触发 calc，仅靠 calc-done 永远不会 reconcile。

> **接受判据 = import-RT 匹配，而非 FIB 下一跳可达性。** `pick_import_source` 不要求
> `BGP_ROUTE_FLAG_VALID`，否则在无 LSP 的拓扑里收到的 vpnv4 路由因 PE 下一跳 Unresolved 永远无法导入。
> 这与“转发（隧道/标签）属后续工作”一致：导入路由进 VRF Loc-RIB 可见、可向 CE 再通告，FIB 转发待补。

合成来源地址：`AF_INET6`，`s6_addr[0..7]=源 RD`，使同前缀经不同 RD 导入同一 VRF 时成为
各自独立的竞争路径，且与该 VRF 内真实 IPv4 peer 来源天然区分。

### 4. 指针生命周期

导入节点（VRF unicast RIB 内、`BGP_ROUTE_FLAG_IMPORT`、合成来源=源 RD）的 `src_route`
指向其来源 vpnv4 best 节点并 `borrow_ref` 钉住。撤销/best 切换/源 inst 销毁时统一在本子系统内
`detach`：
- best 切换/消失：`reconcile_head` 的 upsert/withdraw 路径。
- import-RT 配置变更：`bgp_vrf_import_backfill()` 扫公网 vpnv4 RIB 逐 head reconcile。
- public vpnv4 instance 销毁：`bgp_vrf_import_purge_target_inst()`（`bgp_instance_destroy` 在
  vpnv4 RIB 释放前调用，此时源节点仍存活）。

与 export 的 borrow 约定一致。

## 改动文件

| 文件 | 改动 |
|------|------|
| `bgp_vrf_import.{c,h}` | 新增：IRT 索引 + 过滤判据 + reconcile + backfill + purge |
| `bgp_ext_community.{c,h}` | 暴露 `bgp_ext_community_rt_canon()` / `bgp_ext_community_is_rt()` |
| `bgp_relay.c` | ingest 增加 vpnv4 入向 RT 过滤 + 入库成功后显式触发导入评估 |
| `bgp_calc.c` | 三处 calc-done 加 `bgp_vrf_import_on_calc_done` |
| `bgp_apply_vrf.c` | IMPORT_RT_ADD/DEL → 维护索引 + backfill；VRF_DEL/AF_DISABLE → purge |
| `bgp_instance.c` | destroy 加 `bgp_vrf_import_purge_target_inst` |
| `bgp_worker.c` | 启动 `bgp_vrf_import_init` |
| `bgp_cmd.c` | VRF_DOWN 加 `bgp_vrf_import_purge_all` |
| `bgp/CMakeLists.txt` | 加入新源文件 |

## 当前限制（后续工作）

- 导入节点的**转发**（基于 received VPN 标签的隧道下一跳迭代）尚未实现：路由已正确灌入
  VRF Loc-RIB 并携带 received 标签（`BGP_ROUTE_LABEL_SOURCE_RECEIVED`），但 FIB 下刷会按
  best 的 PE 下一跳直接编程，缺少标签/隧道封装。与 export 侧发送编码器未完成相对应。
- 仅 vpnv4（afi=ipv4）。vpnv6 导入未实现。
- reconcile 每 head 遍历全部私网 VRF（O(V)）；后续可加 RT→VRF 反向索引优化。
