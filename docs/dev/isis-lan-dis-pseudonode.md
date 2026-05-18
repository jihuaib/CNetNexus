# ISIS LAN：DIS 选举与伪节点 LSP 处理流程

本文记录 NetNexus ISIS 在 LAN 类型链路（PDU type 15/16, LAN IIH）上的 DIS 选举与伪节点（Pseudonode）LSP
生成/泛洪流程，配合 `scripts/ci/modules/interop/isis-frr/n2-l1-g1`（NetNexus ↔ FRR 直连）这个最小组网说明每一步。

---

## 1. 为什么 LAN 需要 DIS + 伪节点

点对点链路（P2P）上只有两台 IS，互相在自己的 LSP 里写"我能到对端 system-id"即可。
广播/LAN 链路（以太网）原则上可以接 N 台 IS，如果每台都在 LSP 里写"我和另外 N-1 台都有邻接"，
邻接条数会膨胀到 O(N²)，LSDB 和 SPF 都会被拖垮。

ISO 10589 / RFC 1195 的做法是引入**伪节点（Pseudonode）**：

```
真实拓扑：r1 ─┐
            ├─ 同一个交换机/桥
            r2 ─┤
            r3 ─┘

ISIS SPF 看到的拓扑：
            r1 ─┐
            r2 ─┼─ <DIS>.<circuit-id>（伪节点）
            r3 ─┘
```

每台路由器只写一条 IS reach："我连到伪节点 `<DIS-sysid>.<circuit-id>`"。
伪节点再写一条 LSP，列出该 LAN 上所有路由器（含自己），每个 metric=0。
邻接条数从 O(N²) 降到 O(N)。

**DIS（Designated Intermediate System）** 是被选出来"代表 LAN 发伪节点 LSP"的那台路由器。
选举规则：

- LAN Priority 高者赢
- Priority 相同时，SNPA（接口 MAC）字典序大者赢

---

## 2. 参考组网

`scripts/ci/modules/interop/isis-frr/n2-l1-g1/top.yaml`：

```
+----+     GE-1/eth1     +----+
| r1 |---10.12.0.0/30---| f1 |
|NetN|  2001:db8:12::/64|FRR |
+----+                  +----+
```

- r1（NetNexus）：`net 49.0001.0000.0000.0001.00`，is-type level-1-2，cost-style wide，IPv4+IPv6
- f1（FRR）：`router isis 1; net 49.0001.0000.0000.0002.00; is-type level-1-2; metric-style wide`
- GE-1/eth1 共桥，两端都开 ISIS（LAN 模式，FRR 不加 `isis network point-to-point`）

为了让 SPF 在两端都能把对端学到 RIB，最少要完成下面 4 件事，本文按顺序拆解。

---

## 3. 数据结构

`src/isis/work/isis_worker.h`：

```c
typedef struct isis_dis_state {
    uint8_t lan_id[7];           // 当前公认的 DIS LAN-ID = sysid(6B) + circuit-id(1B)
    uint8_t we_are_dis;          // 我们赢了选举？
    uint8_t our_circuit_id;      // 自己做 DIS 时使用的 circuit-id（非 0）
    uint64_t last_election_msec;
    uint32_t pseudo_seq;         // 自己做 DIS 时发出的伪节点 LSP 序列号
} isis_dis_state_t;

typedef struct isis_if_cfg {
    ...
    isis_dis_state_t dis_l1;     // 该接口 L1 的 DIS 状态
    isis_dis_state_t dis_l2;     // 该接口 L2 的 DIS 状态
} isis_if_cfg_t;

typedef struct isis_neighbor {
    ...
    uint8_t priority;            // 邻居在 IIH 里声明的 priority
    uint8_t remote_snpa[6];      // 邻居 SNPA（用于打平 priority 时的字典序比较）
    uint8_t remote_lan_id[7];    // 邻居 IIH 里写的 LAN-ID（它认的 DIS 是谁）
    ...
} isis_neighbor_t;
```

每接口每 level 有独立的 DIS 状态。

---

## 4. IIH 报文里的 DIS 相关字段

LAN IIH（PDU type 15 = L1 / 16 = L2）头部 27 字节，关键偏移：

```
offset  字段
   0    NLPID (0x83)
   8    Circuit Type
   9..14 Source System ID
  15..16 Hold Time
  17..18 PDU Length
  19    Priority (1B)
  20..26 LAN-ID (sysid 6B + circuit-id 1B)
  27+   TLVs (Area 1, Protocols Supported 129, IS Neighbors 6 等)
```

`Priority`：本机愿意做 DIS 的优先级（NetNexus 当前固定 64，FRR 默认也是 64）。
`LAN-ID`：**本机当前认为的 DIS 的 LAN-ID**。两端的 LAN-ID 收敛一致才算选举完成。

---

## 5. 发送侧：IIH TX 选 LAN-ID

代码位置：`src/isis/work/isis_neighbor.c::isis_build_iih_pdu`。

```c
pdu[p++] = 64u;  // priority

const isis_dis_state_t *dis = (level == 1u) ? &if_cfg->dis_l1 : &if_cfg->dis_l2;
uint8_t lan_id_out[7];
if (dis->lan_id[6] != 0u || dis->we_are_dis)
{
    // 已经跑过选举：填入 elected DIS 的 LAN-ID
    memcpy(lan_id_out, dis->lan_id, 7u);
}
else
{
    // 启动初期还没收敛：暂时填本机 sysid + 本机 circuit-id 占位，
    // 让对端能用我的 (sysid, snpa) 跟自己比一比，加快收敛
    memcpy(lan_id_out, system_id, 6u);
    lan_id_out[6] = isis_dis_circuit_id_for(if_entry);
}
memcpy(&pdu[p], lan_id_out, 7u);
```

`circuit-id` 由接口的 `if_entry->ifindex` 派生（低 8 位，0 时退化到 1），保证一台路由器多个 LAN 接口下
互不冲突。

---

## 6. 接收侧：IIH RX → 触发选举

代码位置：`src/isis/work/isis_neighbor.c::isis_handle_iih_payload`。

```c
ctx.priority      = pdu[19];
memcpy(ctx.remote_lan_id, &pdu[20], 7u);
...
// 解析完所有 TLV、确定 hello_valid / new_state 之后，更新邻居记录：
nbr->priority = ctx.priority;
memcpy(nbr->remote_snpa, ctx.remote_snpa, 6u);
memcpy(nbr->remote_lan_id, ctx.remote_lan_id, 7u);

// 任何邻居状态变更都重新跑一次 DIS 选举
isis_dis_run_election(inst, ctx->ifname, ctx->level, ctx->now_msec);
```

为防止启动初期没收到任何 IIH 就先发自己的 IIH 时出现 `lan_id=00...`，
hello TX 的 callback 在调用 `isis_send_iih_on_if` 之前也跑一次选举：

```c
// isis_send_hello_if_cb
if (isis_level_enabled(ctx->inst, 1u)) {
    isis_dis_run_election(ctx->inst, if_cfg->ifname, 1u, ctx->now_msec);
    isis_send_iih_on_if(ctx->inst, if_cfg, if_entry, 1u);
}
```

这样即使没有邻居，"自己跟自己比"也会得出"自己是 DIS"，IIH 携带的 LAN-ID 立刻有效。

---

## 7. 选举算法

`isis_dis_run_election`：

```c
候选集合 = {本机} ∪ {该 (ifname, level) 上所有 UP 邻居}
winner = 候选中 max(priority, snpa)

if winner == 本机:
    dis->we_are_dis = 1
    dis->lan_id = <本机 sysid> + <本机 circuit-id>
else:
    dis->we_are_dis = 0
    if 邻居的 remote_lan_id[6] != 0:     # 邻居告诉我 DIS 是谁，优先采纳
        dis->lan_id = 邻居的 remote_lan_id
    else:                                # 兜底：用 winner 的 sysid + 1
        dis->lan_id = <winner sysid> + 1

if 刚刚转为 DIS:
    dis->pseudo_seq += 1                 # 触发下个 tick 重发新版伪节点 LSP
```

NetNexus 当前 priority 固定 64；和 FRR 对比时 SNPA 字典序决定胜负。在我们的组网里 r1 SNPA 比 f1 大，
所以 r1 当选 DIS，LAN-ID = `0000.0000.0001.03`（r1 sysid + 它给 GE-1 分配的 circuit-id=3）。

---

## 8. 关键 TLV：Area + Protocols Supported

代码位置：`src/isis/work/isis_lsp.c::isis_lsp_build_pdu`。在 IS/IP reach 之前先写：

```c
// TLV 1 Area Addresses
isis_lsp_extract_area(inst->net, area, ...);
isis_lsp_append_single_tlv(pdu, ..., ISIS_TLV_AREA_ADDR, area_val, area_len+1);

// TLV 129 Protocols Supported (RFC 1195 NLPID 列表)
if (inst->af_ipv4) nlpids[n++] = 0xCC;   // IPv4
if (inst->af_ipv6 && !narrow) nlpids[n++] = 0x8E;  // IPv6
isis_lsp_append_single_tlv(pdu, ..., ISIS_TLV_PROTOCOLS_SUPPORTED, nlpids, n);
```

**为什么必备**：FRR/IOS-XR 这类实现的 SPF 拿到一份 LSP 时，会先校验

- 是不是同 area（用 TLV 1 对比）
- 是不是讲 IP 的节点（用 TLV 129 判断）

任一缺失，整个 LSP 会被当成"不能讲 IP 的纯 ISO CLNS 节点"，SPF 跳过 → 拿不到对端前缀。

实际抓的 LSDB（早期版本缺这两个 TLV 时，FRR 的 `show isis topology` 完全看不到 NetNexus 顶点）。

---

## 9. 普通 LSP 的 IS reach：指向伪节点

代码位置：`src/isis/work/isis_lsp.c::isis_lsp_collect_is_reach_per_if_cb`。

按接口聚合，而不是按邻居：

```c
对每个有 UP 邻居的接口:
    dis = (level==1) ? &if_cfg->dis_l1 : &if_cfg->dis_l2
    if dis 尚未收敛: 跳过
    target_lan_id = dis->lan_id
    if dis->we_are_dis && target_lan_id[6] == 0:
        target_lan_id[6] = dis->our_circuit_id

    向 v4_entries 写 1 条 IS reach：
        sysid = target_lan_id[0..5]
        pseudonode-id = target_lan_id[6]    # 关键：非 0
        metric = if_cfg 的本端 metric
```

观察 FRR 的 `show isis database detail`：

```
0000.0000.0001.00-00       <- r1 自己的常规 LSP
  Protocols Supported: IPv4, IPv6
  Area Address: 49.0001
  Extended Reachability: 0000.0000.0001.03 (Metric: 10)   <- 指向伪节点
  Extended IP Reachability: 10.255.1.1/32 (Metric: 10)
  ...

f1.00-00                   <- f1 自己的常规 LSP
  Protocols Supported: IPv4, IPv6
  Area Address: 49.0001
  Extended Reachability: 0000.0000.0001.03 (Metric: 10)   <- 同一个伪节点
  Extended IP Reachability: 10.255.2.2/32 (Metric: 10)
  ...
```

两端都指向 `0000.0000.0001.03` — 但这个伪节点本身要有一份 LSP，SPF 才能"走过去"。

---

## 10. 伪节点 LSP 的构造与泛洪

代码位置：`src/isis/work/isis_lsp.c::isis_lsp_build_pseudonode_pdu`。

只有 `dis->we_are_dis == 1` 的 (interface, level) 才发：

- **LSP-ID**：`<我的 sysid>.<my_circuit_id>-00`
  - sysid（6B） + pseudonode-id（1B 非 0） + fragment（1B，目前固定 0）
- **TLV**：
  - `Area Addresses (TLV 1)`
  - `Protocols Supported (TLV 129)`
  - `Extended IS Reachability (TLV 22)` 或 narrow `IS Reachability (TLV 2)` — 含 LAN 上**全部**路由器（自己 + 所有 UP 邻居），每个 entry 的 `<sysid> + <pseudonode-id=0>`，metric **必须 0**

伪节点 LSP 不含 IP/IPv6 reach（IP 前缀都在各路由器自己的常规 LSP 里）。

发送时机：`isis_lsp_send_due` 主循环里，发完常规 L1/L2 LSP 后，再扫一遍 `inst->if_cfgs`：

```c
foreach if_cfg:
    foreach level in {1, 2}:
        dis = (level==1) ? &if_cfg->dis_l1 : &if_cfg->dis_l2
        if !dis->we_are_dis: continue
        if !isis_lsp_if_has_up_adjacency(...): continue   # 没邻居就不发，免得污染对端 LSDB
        pn_seq = dis->pseudo_seq + 1
        build pseudonode LSP into pn_pdu
        send_pdu_on_if(...)
        dis->pseudo_seq = pn_seq
```

序列号独立于常规 LSP 的 `inst->lsp_seq_l1/l2`，避免相互影响。

---

## 11. SPF 视角：可见性链条

SPF（`isis_spf.c::isis_spf_collect_graph_from_lsdb`）遍历 LSDB 把 IS reach 转成有向图边。
对方实现（FRR）的 SPF 也是同理。

要让 FRR 学到 r1 的 `10.255.1.1/32`，必须形成完整可见性链：

```
1. FRR 看到自己 (f1.00-00) 的 IS reach → 指向 0000.0000.0001.03
2. FRR 在 LSDB 找到 0000.0000.0001.03-00 → 发现里面列了 0000.0000.0001 和 f1
3. FRR 走过去到 0000.0000.0001.00-00 → 拿到 Extended IP Reachability: 10.255.1.1/32
4. 安装路由
```

**任何一步缺失**：

| 缺失 | 表现 |
|---|---|
| 第 1 步（IS reach 写错成 sysid+0 而非伪节点 LAN-ID） | FRR LSP 里没有 IS reach；FRR `show isis topology` 只看到自己 |
| 第 2 步（伪节点 LSP 没生成） | r1 / f1 的常规 LSP 都指向 `0000.0000.0001.03`，但 FRR `show isis database` 没有这条 → SPF 中断 |
| 第 3 步 LSP 缺 TLV 1/129 | FRR 把 LSP 起点当成"不讲 IP" → 跳过整个起点 |
| 第 4 步（前缀 TLV 解析错） | 例如 TLV 135 头部 5 vs 6 字节弄反 → prefix_len 错位、丢前缀 |

这套实现把前 3 步都补齐了；第 4 步在更早的 commit 修了（5/6 字节头的 IPv4/IPv6 reach 拆开解析）。

---

## 12. show 命令验证

### 12.1 `show isis interface ipv4|ipv6 <tag>`

新版输出会在每个接口 AF 行之后追加 L1/L2 的 DIS 行：

```
ISIS Interfaces
Tag 1 (level-1-2)
  GE-1             ipv4  metric=10       hello=3    hold-mult=3   passive=0
  GE-1             L1   dis=0000.0000.0001.03  [we-are-DIS]
                       our-circuit-id=3 pseudo-lsp-seq=5
  GE-1             L2   dis=0000.0000.0001.03  [we-are-DIS]
                       our-circuit-id=3 pseudo-lsp-seq=5
  loop11           ipv4  metric=10       hello=10   hold-mult=3   passive=1
```

- `dis=0000.0000.0001.03`：当前公认的 LAN-ID，前 12 个 hex 是 DIS sysid，最后 2 个是 circuit-id
- `[we-are-DIS]`：本机赢了选举（否则没这行）
- `our-circuit-id`：我们给这个 LAN 分配的 circuit-id（与 `dis->lan_id[6]` 一致）
- `pseudo-lsp-seq`：已经发出去的最大伪节点 LSP 序列号

passive 接口不参与选举（loopback 没有 LAN 语义）。

### 12.2 `show isis lsdb ipv4|ipv6 <tag>`

LSDB detail 现在能解析 TLV 1 / 129 / 132：

```
LSP Entry 2
  System-ID       : 0000.0000.0001
  TLV[1]         : type=129 (Protocols-Supported) len=2
    NLPIDs        : IPv4(0xcc) IPv6(0x8e)
  TLV[2]         : type=1 (Area-Addresses) len=4
    Area[1]       : 49.0001
  TLV[3]         : type=22 (Extended-IS-Reachability) len=11
    IS[1]         : neighbor=0000.0000.0001.03 metric=10 ...     <- 指向伪节点
  TLV[4]         : type=135 (Extended-IP-Reachability(IPv4)) len=...
    IPv4[1]       : prefix=10.255.1.1/32 metric=10
```

伪节点 LSP（pseudonode-id ≠ 0）的展示同样走这套解析，只不过 IS reach 里的 entry 是直连邻居 + 自己。

---

## 13. 已知简化项 / TODO

- **DIS Priority 固定 64**：尚未做 CLI 配置（`isis dis-priority <0-127>`）
- **Hold timer 不影响 DIS 衰减**：邻居 Down 后立刻重选；没做"延迟若干秒再重选"以减少抖动
- **Two-way check 简化**：当前以 `nbr->state == UP` 为唯一条件，未严格区分 ISO 10589 §8.4.5 中 "DIS 候选" 与 "完整邻接" 的差别（小规模 LAN 上等价）
- **伪节点 LSP fragment**：固定 fragment=0，不分片；超出 1500B 会被 `isis_lsp_build_pseudonode_pdu` 直接拒发，目前 LAN 邻居数有限不会触发
- **purge on DIS loss**：从 DIS 退位时未显式发送 lifetime=0 的伪节点 LSP；依赖对端 ~20min 自然 age-out（厂商通常会显式 purge，可作为后续优化）

---

## 14. 相关文件索引

| 文件 | 内容 |
|---|---|
| `src/isis/work/isis_worker.h` | `isis_dis_state_t`、`isis_neighbor_t` 加 `remote_lan_id` |
| `src/isis/work/isis_neighbor.c` | `isis_dis_run_election`、IIH TX 写 LAN-ID、IIH RX 触发选举 |
| `src/isis/work/isis_lsp.c` | TLV 1/129 emit、`isis_lsp_collect_is_reach_per_if_cb`、`isis_lsp_build_pseudonode_pdu`、`isis_lsp_send_due` 伪节点泛洪段 |
| `src/isis/work/isis_show.c` | `show_dis_line`、`lsdb_parse_area_tlv`、`lsdb_parse_protocols_tlv`、`lsdb_parse_ipv4_intf_addr_tlv` |
| `scripts/ci/modules/interop/isis-frr/n2-l1-g1/isis_dual_stack_basic.py` | 端到端互通验证用例 |
