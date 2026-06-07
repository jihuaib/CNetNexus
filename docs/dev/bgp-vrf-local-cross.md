# BGP VRF 本地交叉（Local Route Leaking）

## 1. 需求与定位

同一台设备上两个私网 VRF 之间需要互通（共享服务 / extranet）时，传统做法要绕 vpnv4：
私网 unicast → `bgp_vrf_export` 灌 vpnv4 → eBGP 发对端 PE → 对端 `bgp_vrf_import` 按 RT 导入。
**本地交叉**让本机 vrf1 的 ipv4-unicast 路由按 vrf1 的 **export-RT** 直接命中本机其它 VRF 的
**import-RT**，命中即把该前缀作为合成路径插入目标 VRF 的 unicast RIB 并下刷 FIB，实现真实跨表转发。

特性约束（设计决策）：
- **完全独立于 vpnv4**：无需使能 vpnv4、无需任何 BGP 邻居。仅靠 VRF 的 RT 配置驱动。
- **单跳不传递**：已泄漏（`LOCAL_CROSS`）/远端导入（`REMOTE_CROSS`）的路由不再作为泄漏源，防环。
- **控制面 + 转发**：既插入 RIB，也下刷 FIB 完成数据面转发。

实现并入 `src/bgp/work/bgp_vrf_import.c/.h`（复用其 IRT 索引与 reconcile/borrow 机制），未新建模块。

## 2. 标记位复用

复用 `BGP_ROUTE_FLAG_LOCAL_CROSS (1U<<7)`（`bgp_rib.h`），语义扩展为“本地跨表合成路由”，按**所在
instance** 区分两种来源：
- public vpnv4 inst 内：vrf-export 产物（私网 unicast → vpnv4）。
- 私网 unicast inst 内：本地交叉泄漏（本机另一 VRF → 本 VRF）。

二者都属本地起源、可正常对外通告，已被 `bgp_route_is_local_origin()` / `bgp_route_is_synthetic()` 覆盖。

## 3. 控制面（RT 匹配 + 插入）

- **IRT 索引** `g_bgp_irt_index`（import-RT → {vrf_id}）由 VRF `AF_IMPORT_RT_ADD/DEL` 事件维护，
  独立于 vpnv4，本地交叉直接复用。
- **泄漏源**：私网 VRF ipv4-unicast 的 **VALID best**，跳过 `LOCAL_CROSS`/`REMOTE_CROSS`（单跳防环）。
- **目标集合**：`local_collect_targets(src_vrf_id)` 读源 VRF 的 export-RT（`vrf_api_cache_get_af`），
  规范化后查 IRT 索引，收集命中的 target vrf_id（排除源自身与 public）。
- **合成来源键** `synth_source_from_local_vrf(src_vrf_id)`：AF_INET6，`s6_addr[8]=0x4C` 标记 +
  `s6_addr[12..15]=htonl(src_vrf_id)`，与 REMOTE_CROSS 的 RD 键（`s6_addr[8..15]=0`）、真实 peer 键
  均不冲突，且不同源 VRF 泄漏同前缀互不覆盖（各成竞争路径）。
- **reconcile** `local_reconcile_head`：取源 best → 求目标集合 → 遍历私网 VRF：命中 `local_upsert`，
  否则 `import_withdraw`（复用 vpnv4 import 的撤销框架）。

### 触发点（hook）
- `bgp_calc.c` 三处 calc-done：`bgp_vrf_import_local_on_calc_done`（best 变化即重评）。
- `bgp_cfg_apply.c` 私网 unicast 实例**创建**时：`bgp_vrf_import_local_backfill_target_vrf`
  （新建 VRF 是其它 VRF 路由的潜在泄漏目标，但其 `bgp_vrf_t` 在源 calc 时尚不存在、无法 upsert
  进来；实例就绪后全量重评把已有路由补泄漏进来——**这是初配时序的关键修复**）。
- `bgp_apply_vrf.c`：`IMPORT_RT_ADD/DEL` → `local_backfill_target_vrf`；
  `EXPORT_RT_ADD/DEL` → `local_backfill_source_vrf`。
- `bgp_instance.c::bgp_instance_destroy`：私网 unicast inst 销毁前 `bgp_vrf_import_local_purge_inst`
  双角色清理（作为源撤其它 VRF 内泄漏、作为目标解 borrow）。

### 防环
`bgp_vrf_export.c::process_one` 将 `LOCAL_CROSS` 加入“不可导出回 vpnv4”判据（同 `REMOTE_CROSS`），
避免本地泄漏路由又被某 VRF 用自己 RD 推回 vpnv4。

## 4. 转发（nexthop-vrf 模型，关键）

本地交叉采用 FRR 同类的 `nexthop-vrf` 语义：路由安装在**目标 VRF**，但下一跳递归解析发生在
**源 VRF**。这样源 VRF 中经真实 CE 网关可达的前缀泄漏到目标 VRF 后，目标 VRF FIB 仍指向真实网关
与真实出接口，而不是指向 VRF 主设备。

实现要点：
- `local_upsert` 为目标 VRF 内的 LOCAL_CROSS 路由申请自有 BGP nexthop 对象。
- `bgp_nexthop_make_route_key` 对私网 unicast 的 LOCAL_CROSS 特判：nexthop 对象的 `key.vrf_id`
  使用 `src_route` 所在的源 VRF。
- ROUTE 因此在源 VRF 内迭代该 nexthop，解析出真实 `Iter NH / Iter OIF`；BGP 收到解析结果后把
  LOCAL_CROSS 路由下刷到目标 VRF FIB。
- `local_upsert` 必须继承源 best 的原始 nexthop 作为迭代目标；源路由没有可用 nexthop 时保持未解析，
  不能用目的前缀伪造递归目标。

转发路径（例：blue 泄漏 red 中经 CE 网关可达的 `100.2.2.2/32`）：
```
blue 表查 100.2.2.2 → LOCAL_CROSS 路由
LOCAL_CROSS nexthop 在 red 表递归解析 → 10.0.12.2 / GE-1
blue FIB 安装 100.2.2.2 via 10.0.12.2 dev GE-1
```

## 5. CI 验证

`scripts/ci/modules/bgp/n2-l1-g1/vrf_local_leak.py`：
- r1 上 red/blue 互配 `vpn-target 1:1 export/import`，**不使能 vpnv4、无邻居**；
- red 内通过静态路由经真实网关 `10.0.12.2` 到 r2 loopback `100.2.2.2/32`；
- 控制面：blue 的 BGP 表出现 red 泄漏来的 `100.2.2.2/32`（`LOCAL_CROSS`）；
- 转发面：blue 的 FIB 显示 `Iter NH 10.0.12.2`、`Installed yes`、`NH-Type ip`；
- 数据面：`ping 100.2.2.2 -a 100.1.1.1 vrf blue` 成功。

跑法：
```
docker build --target debug --build-arg BUILD_TYPE=Debug -t netnexus-ci:vrflocal .
python3 scripts/ci/module_runner.py --script scripts/ci/modules/bgp/n2-l1-g1/vrf_local_leak.py --image netnexus-ci:vrflocal
```
回归通过：`vpnv4_route_exchange` / `vpnv4_traffic` / `import_route`。

## 6. 注意事项 / 排查坑

- BGP VRF af 实例创建要求先配 RD（`bgp_cfg_apply.c` 既有约束，与 vpnv4 无关）；本地交叉本身不用 RD。
- 初配时序：源 VRF 路由先于目标 VRF 的 `bgp_vrf_t` 存在 → 源 calc 时目标不在 `proto->vrf_hash`，
  无法 upsert；靠目标实例创建时的 `local_backfill_target_vrf` 补齐。
- 出接口来自源 VRF 内对原始 nexthop 的递归解析结果；connected/onlink 场景也不能改用目的前缀迭代。
- CI 容器调试：access console 仅 1 条 line，断开不干净会“All lines are busy”，需
  `docker exec -u 0 <ctr> pkill -9 -f netnexus-console` 释放；`--pause-on-fail` 后 runner 仍会
  post-cleanup 截断 `/opt/netnexus/log/`，看日志去 `scripts/ci/reports/containers/.../modules/bgp.log`。

## 7. 后续工作

- 仅 ipv4-unicast；ipv6-unicast 未做。
- `local_backfill_target_vrf` 为全量重评（O(所有源路由)），可加反向索引优化。
- 已用 CE 网关场景验证 nexthop-vrf 递归解析；纯 connected/onlink 的原始 nexthop 语义仍需继续补 CI。
