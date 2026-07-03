# BGP vpnv4 使能时 VRF 路由批量导出设计

> 状态:P1 已落地(framework + enable 全量分批 + process_one + 增量 + disable)
> 作者:jhb
> 日期:2026/05/31
> 关联模块:`src/bgp/`(work 层)、`src/vrf/`(元数据)、`src/tunnel/`(MPLS,按需)
> 子系统命名:`bgp_vrf_export`(文件 `work/bgp_vrf_export.c/.h`)

## 0. 实现与本设计的差异(P1 落地记录)

- 命名采用 `bgp_vrf_export`(非 `bgp_vpn_export`),与 `bgp_vrf.c`/`bgp_apply_vrf.c` 同族;
  表示导出目标实例的标识统一为 `vpn_inst`(非 `vpnv4_inst`),为后续 EVPN/vpnv6 复用预留。
- **导出节点持有源路由前向指针(溯源)**:导出 `bgp_route_node_t.src_route` 指向当前 best 源节点,
  并 `bgp_route_node_borrow_ref` 钉住源节点。这样可知"某条 VPN 路由由哪条 VRF 路由生成",
  支撑后续按源增量/撤销/show 溯源/策略。
  (复用通用 borrow 机制:源节点被 unreach/RIB 销毁时若仍被借用则置 `PENDING_FREE`,
   待 `borrow_unref` 真正释放;`src_route` 不被通用 free 路径解释,与 import_rib mirror 互不干扰,
   因导出节点用 `BGP_ROUTE_FLAG_IMPORT` 而非 `IMPORT_RIB`。)
- **不存"源→导出"反向表**:早期版本曾用 `exported_by_src` 哈希,但它存裸导出节点指针、
  无 borrow 保护也无 free 通知,导出节点被回收即悬空;且 src→导出可由"源 head + VRF RD 现推
  vpn_nlri 再查 vpnv4 RIB"得到,无需存指针。故删除。**唯一长期持有的跨节点指针 = export->src_route**,
  受 borrow 保护,释放只走单一入口 `detach_src`(先置空 src_route 再 unref，幂等)。
- 导出路由在 vpnv4 RIB 内以 **per-VRF 合成来源**(`AF_INET`,地址=`htonl(vrf_id)`)为键
  (每 (rd, prefix) 一条导出,始终跟踪当前 best),保证与"远端 vpnv4 邻居学习"路由并存共选。
- **生命周期(持指针的代价,已落地)**:
  - process_one best 切换来源时换 borrow(detach 旧 + ref 新);best 消失时 detach + unreach。
  - `bgp_vrf_export_inst_destroy`:扫本 inst 的 vpnv4 RIB(尚未销毁),对所有 IMPORT 节点 detach。
  - `bgp_vrf_export_disable`:逐导出节点 detach + unreach。
  - `bgp_vrf_export_purge_source_inst(src_inst)`:私网 VRF unicast 实例销毁前(`bgp_instance_destroy`
    顶部调用,源 RIB 尚存活),扫 vpnv4 RIB 找 `src_route->head->inst == src_inst` 的导出节点,
    detach + unreach —— **避免源节点被钉成 PENDING_FREE 孤儿**。
  - 所有释放统一经 `detach_src`(单一入口、幂等);全文件仅一处 `borrow_ref`(process_one 挂接时)。
- **vpnv4 报文编码器已补齐**:`work/bgp_pkt_build_vpn.c`(afi=1/safi=128),与 `lib/bgp_parse_vpn.c`
  线格式对称:NLRI = length-bits(88+plen) + label(3B) + RD(8B) + prefix;MP_REACH nexthop = RD(8B 全0)
  + IPv4(4B)=12B。在 `bgp_pkt_build_init` 注册。announce 路径:vpnv4 inst calc→best→enqueue_announce→
  update_group `build_packed_update(afi=1,safi=128)`→`enc_find`→本编码器。
  **next-hop-self 自动正确**:导出节点带 `BGP_ROUTE_FLAG_IMPORT` → `bgp_classify_route_src`=RSRC_IMPORT
  → `bgp_select_nh_rule`=R_LOCAL,对 eBGP/iBGP 都用本端地址替换 nexthop(L3VPN 正确行为),无需额外处理。
- **VRF 事件联动已完成**(`bgp_apply_vrf.c`):
  - 撤销侧:`VRF_DEL`(`on_vrf_del`→`g_hash_table_remove(vrf_hash)`→`bgp_vrf_destroy`→`bgp_instance_destroy`)、
    `AF_DISABLE`/`AF_RD_DEL`(→`bgp_vrf_del_instance`→`bgp_instance_destroy`)都经 instance_destroy →
    `purge_source_inst` 覆盖。
  - 补灌侧:`AF_RD_ADD`(ipv4-unicast)→ `bgp_vrf_export_backfill_vrf(vrf_id)`,把该 VRF 已有路由扫入
    pending 重新导出(覆盖"vpnv4 先使能、VRF 后配 RD,全量扫描时因无 RD 被跳过"的时序)。
- 三条时序全闭环:①routes+RD 在先→vpnv4 enable 全量扫描;②vpnv4 在先→RD add 补灌;③两者在先→route 到来 on_calc_done。
  idempotent(按 synth source 键)保证不重复导出。
- **持久化已自然覆盖(无需额外工作)**:vpnv4 使能态走 BGP 通用实例持久化——`bgp_db_set_instance`
  写 `bgp_instance` 行,`bgp_bdr.c` 回放 ` af vpnv4`,重启回放经 `bgp_cfg_apply_instance` 触发
  `bgp_vrf_export_enable` 自动重导出;apply-label 模式已在 VRF DB 持久化;per-vrf 标签按设计发送时
  重新申请(不持久化);导出路由由源 VRF unicast 重新导出(不持久化)。
- **未实现(按需)**:per-route 标签模式(配置/存储/显示链路已全,仅缺 per-prefix 申请逻辑)。

## 1. 需求

当 public VRF(`vrf_id=0`)下使能 `vpnv4`(afi=ipv4, safi=vpn-unicast)地址族时,
要把**所有私网 VRF(`vrf_id≠0`)的 ipv4-unicast 最优路由**导出到 vpnv4 表,
按各私网 VRF 配置的 **RD** 存进 vpnv4 instance 下对应的 `bgp_rd_entry` RIB。
这是经典 L3VPN 的 **VRF→VPN 导出(VPN export / route leaking)**。

路由量可能巨大,使能瞬间不能一次性灌完阻塞 worker,**必须分批处理**。

### 已确认决策

| 项 | 决策 |
|---|---|
| VPN label 分配粒度 | **per-VRF 单标签**(默认);配置项 `apply-label per-vrf\|per-route` 放在 **VRF 模块**(经事件下发 BGP),本轮只实现 per-vrf 行为 |
| VPN label 申请时机 | **发送时**向 TUNNEL 申请(`tunnel_rpc_label_alloc`,source=BGP_ADJ,per-vrf 聚合 fec.prefix_len=0),缓存到 `bgp_vrf.vpn_label`;**loc-rib 不带标签**;申请不到则 hold(不通告),标签可得后下次 pub 再发 |
| 导出范围 | **peer 学习 + import-route 本地引入(static/直连)** 的 VALID best 全部导出 |
| 本文交付 | 仅设计文档,确认后再分阶段编码 |

## 2. 现有可复用基础设施

| 设施 | 位置 | 复用点 |
|---|---|---|
| import_rib mirror 框架 | `work/bgp_import_rib.c` | pending queue + reverse map + 批处理范式,结构同构照搬 |
| RD entry 按 NLRI ensure | `bgp_inst_rib_ensure_for_nlri()` | VPN AF 自动按 NLRI 的 RD 建 `rd_entry`/RIB |
| export RT 合成 | `bgp_ext_community_merge_vrf_export_rts()` | 导出时把 VRF export RT 合进 ext-community |
| VRF 元数据缓存 | `vrf_api_cache_get_af()` → `af->rd / has_rd / export_rts` | 取每个 VRF 的 RD 与 export RT |
| worker 批处理范式 | `bgp_worker_drain_work_events()` | 每批 `BGP_WORK_BATCH_SIZE`(64) + 剩余 re-signal eventfd 自重排 |
| calc 队列自重排 | `bgp_calc_queue_process()` | mirror 落库后 push 目标 calc_queue 走标准优选/发布 |

`bgp_import_src_t` 已预留 `BGP_IMPORT_SRC_VPN_INST=2`(私网 VRF → 公网 VPN),即本需求语义。

## 3. 为何新建独立子系统而非扩展 import_rib

现有 import_rib 的三个核心假设对 VPN export **不成立**:

| 维度 | import_rib(labeled→unicast) | VPN export(本需求) |
|---|---|---|
| 源/目标 VRF | 同 VRF | **跨 VRF**(私网→public) |
| 拓扑 | 1:1 | **N:1**(多私网 VRF → 单 vpnv4 inst) |
| 目标 NLRI | 仅换 safi,prefix 不变 | **prefix 加 RD**(plain→VPN NLRI) + 写 VPN label |
| 属性变换 | 几乎不变 | **合 export RT + 写 RD + 写 label** |
| 目标 inst 选取 | 同 vrf 查 unicast | 固定 = public vpnv4 inst |

`mirror_by_src` / `derive_target_nlri` / `find_target_inst` 全部内建同 VRF 假设。
**结论:新建 `work/bgp_vpn_export.c/h`**,结构同构于 import_rib,逻辑独立,避免污染 import_rib。

## 4. 数据结构

挂在 public vpnv4 instance 上(仅该 inst 非空):

```c
/* bgp_vpn_export.h */
typedef struct bgp_vpn_export_state
{
    GQueue     *pending;        /* 待处理的源 unicast rthead(入队 bgp_rib_head_ref,出队 unref) */
    uint32_t    pending_count;
    GHashTable *mirror_by_src;  /* 源 bgp_route_node_t* -> vpn mirror bgp_route_node_t* */
} bgp_vpn_export_state_t;
```

`bgp_instance_t` 新增字段:

```c
void *vpn_export_state;  /* bgp_vpn_export 内部状态,仅 public vpnv4 inst 非空 */
```

复用 `bgp_route_node_t` 既有字段:`src_route`(反指源)、`borrow_refcnt`、
`BGP_ROUTE_FLAG_IMPORT_RIB`(标记为导入镜像)、`label`/`label_source`/`has_label`。

### per-VRF VPN label 存放

per-VRF 单标签存到 VRF 维度。两种落点(实现期二选一):

- **方案 A(推荐)**:`bgp_vrf_t` 新增 `uint32_t vpn_label`(0=未分配),由本子系统按需分配。
- 方案 B:由 VRF 模块统一分配并通过 `vrf_api_af_t` 下发,BGP 只读。语义更正,但需改 VRF 模块协议,工作量大。

label 池建议复用/新建简单的递增分配器(如从 16 起,跳过保留值),per-VRF 首次导出时分配,VRF 删除时回收。

## 5. 公共 API(`bgp_vpn_export.h`)

```c
/* 生命周期:由 public vpnv4 inst create/destroy 调用 */
void bgp_vpn_export_inst_init(bgp_instance_t *inst);
void bgp_vpn_export_inst_destroy(bgp_instance_t *inst);

/* CLI 入口 */
int  bgp_vpn_export_enable(bgp_instance_t *vpnv4_inst);   /* 全量扫描 + 投事件 */
int  bgp_vpn_export_disable(bgp_instance_t *vpnv4_inst);  /* 撤销所有 mirror */

/* 增量:私网 VRF ipv4-unicast calc 完成后调用 */
void bgp_vpn_export_on_calc_done(bgp_instance_t *src_inst, bgp_rthead_t *head,
                                 const bgp_route_node_t *old_best,
                                 const bgp_route_node_t *new_best);

/* VRF 事件联动:RD 配置/VRF 删除时调用 */
void bgp_vpn_export_on_vrf_rd_add(uint32_t vrf_id);
void bgp_vpn_export_on_vrf_removed(uint32_t vrf_id);

/* 批处理:worker 事件回调驱动,每批最多 batch 条 */
int  bgp_vpn_export_queue_process(bgp_instance_t *vpnv4_inst, int batch);

/* 同步抽干(drain_pending 路径) */
int  bgp_vpn_export_process_pending(bgp_instance_t *vpnv4_inst);
```

辅助查询:`bgp_vpn_export_target_inst()` 返回 `proto->vrf[0]` 的 `(ipv4, vpn-unicast)` inst,
未使能返回 NULL,作为"vpnv4 是否使能"的判据。

## 6. 触发链路(增量维护)

新增 worker 事件类型 `BGP_WORKER_EVENT_VPN_EXPORT`,统一走
`bgp_worker_drain_work_events()` 批处理 + eventfd 自重排。

| 触发源 | 动作 |
|---|---|
| **CLI: vpnv4 enable** | `bgp_vpn_export_enable()`:遍历所有私网 VRF 的 ipv4-unicast inst 全部 head → ref+push pending → 投 `VPN_EXPORT` 事件(**不在 enable 内同步抽干**) |
| **CLI: vpnv4 disable** | `bgp_vpn_export_disable()`:按 `mirror_by_src` 快照逐个 withdraw,清空 pending,回收 per-VRF label |
| **私网 VRF unicast calc done** | `bgp_calc_route_select()` 末尾(现已调 `bgp_import_rib_on_calc_done`)旁挂 `bgp_vpn_export_on_calc_done()`:若 vpnv4 已使能,ref+push 源 head + 投事件 |
| **VRF_EVENT_AF_RD_ADD** | `bgp_apply_vrf.c::on_af_rd_add` 中:若 vpnv4 已使能,把该 VRF 全部 unicast head 灌入 pending |
| **VRF_EVENT_VRF_DEL / AF_DISABLE** | `bgp_vpn_export_on_vrf_removed()`:撤销该 VRF 对应 RD 下所有 vpn mirror,回收 label |

> 注意:RD 是导出前置条件。enable 时若某 VRF 尚无 RD,跳过该 VRF;
> 之后 RD 配上来触发 `AF_RD_ADD`,再补灌——保证时序无论先后都收敛。

## 7. 单条处理 `bgp_vpn_export_process_one(src_head)`

```
1. src_inst = src_head->inst;校验 src_inst->safi==UNICAST 且 vrf_id≠public
2. src_best = bgp_rib_find_best(src_rib, src_head->nlri)
      - 取 VALID 的 best;peer 与 import-route 路由都导出(已确认)
3. 取 RD: af = vrf_api_cache_get_af(vrf_id, IPV4)
      - 若 !af->has_rd → 跳过该条,记 WARN(无 RD 不能进 VPN 表)
4. 取/分配 per-VRF VPN label(vrf->vpn_label,0 则分配)
5. 构造目标 VPN NLRI:
      { afi=IPV4, safi=VPN_UNICAST, rd=af->rd, prefix=src_head->nlri.prefix }
6. tgt_inst = public vpnv4 inst
   tgt_rib  = bgp_inst_rib_ensure_for_nlri(tgt_inst, &vpn_nlri)  // 自动按 RD 建 rd_entry
7. src_best 缺失/非 VALID:
      → 撤销该 RD 下对应 mirror(bgp_vpn_export_mirror_withdraw)
   src_best 存在:
      mirror = ensure_mirror(tgt_rib, tgt_head, src_best->source)
        - attr = clone(src_best->attr)
        - bgp_ext_community_merge_vrf_export_rts(&attr, vrf_id, IPV4)  // 合 export RT
        - mirror->label = vrf->vpn_label; label_source=LOCAL; has_label=1
        - flags |= IMPORT_RIB|VALID; src_route=src_best; borrow_ref(src_best)
        - mirror_by_src[src_best] = mirror
8. push tgt_inst->calc_queue(vpn_nlri) → 标准优选 → 发布给 vpnv4 邻居
```

mirror 的 nexthop / 时间戳同步沿用 import_rib 的 `mirror_sync_from_src` 范式。
跨 VRF 借用源节点必须 `bgp_route_node_borrow_ref`,withdraw 时 `borrow_unref`,
防止源 RIB 清理(unreach/purge)期间 mirror 悬挂。

## 8. 分批处理(核心)

两层批控,完全复刻现有范式,保证 worker 不被长任务阻塞:

```c
/* worker 事件回调:bgp_worker_drain_work_events() 内 */
case BGP_WORKER_EVENT_VPN_EXPORT:
{
    bgp_instance_t *vpnv4 = bgp_vpn_export_target_inst();
    if (vpnv4)
    {
        int n = bgp_vpn_export_queue_process(vpnv4, 256);   /* 每批最多 256 条 */
        bgp_vpn_export_state_t *st = vpnv4->vpn_export_state;
        if (st && st->pending_count > 0)
            bgp_worker_signal_work_event();  /* 自重排,让出 epoll */
    }
    break;
}
```

- enable 全量扫描可产生几十万条 pending,但**每轮仅 256 条**就让出 worker 处理
  报文/定时器,下一轮 eventfd 续跑,无阻塞。
- 下游 vpnv4 calc_queue 本身也是 `BGP_WORK_BATCH_SIZE` 批 + 自重排,二级流水削峰。
- enable 路径**不**在 CLI 线程/worker 同步抽干(与 import_rib 不同),因为量级可能远大。

## 9. disable / 清理路径

- `bgp_vpn_export_disable()`:复制 `mirror_by_src` 的源指针快照,逐个
  `mirror_withdraw`(从 vpnv4 rd_entry RIB 删 mirror、push calc 撤销/下刷 withdraw、
  对源 `borrow_unref`),清空 pending,回收所有 per-VRF label。
- 空 `rd_entry` 在最后一条 mirror 撤销后是否删除:沿用现有 rd_entry 生命周期
  (`bgp_protocol_remove_rd_entry`),建议保留空 entry 由 inst 销毁统一回收,避免抖动。
- `bgp_vpn_export_inst_destroy()`:抽干 pending、清 mirror_by_src(对源 borrow_unref)、
  释放 state;vpnv4 RIB 随 inst 销毁整体释放。

## 10. 改动文件清单

| 文件 | 改动 |
|---|---|
| `work/bgp_vpn_export.h`(新) | 数据结构 + 公共 API |
| `work/bgp_vpn_export.c`(新) | 全部实现(enable/disable/process_one/queue/mirror/label) |
| `work/bgp_instance.h/.c` | 新增 `vpn_export_state` 字段;create/destroy 时 init/destroy(仅 public vpnv4) |
| `work/bgp_vrf.h/.c` | `bgp_vrf_t` 新增 `vpn_label`;VRF 销毁时回收 |
| `work/bgp_worker.h/.c` | 新增 `BGP_WORKER_EVENT_VPN_EXPORT` 事件类型 + drain 分支 |
| `work/bgp_calc.c` | `route_select` 末尾旁挂 `bgp_vpn_export_on_calc_done()` |
| `work/bgp_apply_vrf.c` | `on_af_rd_add` / `on_vrf_del` / `on_af_disable` 联动 vpn_export |
| `bgp_cli.c` | vpnv4 enable/disable 分支调 `bgp_vpn_export_enable/disable` |
| `work/CMakeLists.txt` | 加入 `bgp_vpn_export.c` |
| `bgp_db.*` / `bgp_bdr.c` | (可选)持久化 vpnv4 使能状态 + per-VRF label,重启 restore 时重灌 |

## 11. 阶段拆分(确认后编码)

- **P1 框架打通**:新文件 + 数据结构 + `BGP_WORKER_EVENT_VPN_EXPORT` + enable 全量分批 +
  process_one(label 用 per-VRF,RD 从缓存取,合 export RT) + 发布给 vpnv4 邻居。
  验证:私网 VRF 有路由 → 使能 vpnv4 → vpnv4 邻居收到带 RD/RT/label 的 VPN 路由。
- **P2 增量与一致性**:`on_calc_done` 增量、`AF_RD_ADD` 补灌、disable 撤销、VRF 删除联动。
  验证:使能后改私网路由 / 删 VRF,vpnv4 表实时收敛。
- **P3 持久化**:DB 存 vpnv4 使能态与 per-VRF label,重启 restore 重灌。
- **P4(可选)收尾**:label 回收复用、空 rd_entry GC、show 命令展示 vpnv4 per-RD 路由。

## 12. 风险与注意点

- **跨 VRF 借用生命周期**:mirror 借用源 VRF 的 route_node,必须 borrow_ref/unref 配对,
  否则源会话清理时 mirror 悬挂。这是最易出错点。
- **时序无关收敛**:enable 与各 VRF 的 RD 配置可能任意先后,靠 enable 全量扫描 +
  `AF_RD_ADD` 补灌双路径保证最终一致。
- **resync/VRF 进程重启**:VRF SMOOTHEND / `bgp_apply_vrf_purge_non_public` 时,
  vpn_export 的 mirror 也要随私网 VRF purge 一并撤销,避免残留陈旧 VPN 路由。
- **label 唯一性**:per-VRF label 在本 BGP 实例内必须唯一,删除 VRF 后回收需确保
  无在途引用(数据面/下刷)才复用。
