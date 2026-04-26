# ISIS 报文、邻居协商与 SPF 代码分析

本文基于当前仓库中的 ISIS 实现做代码级分析，重点回答四件事：

1. ISIS 报文在本项目里是怎么编码和收发的。
2. 邻居发现/邻居协商在当前实现里如何推进。
3. LSP 如何进入 LSDB、如何扩散，以及何时触发重算。
4. SPF 如何把 LSDB 转成可下发的 IPv4/IPv6 路由。

分析范围主要对应这些文件：

| 主题 | 代码落点 |
| --- | --- |
| 模块生命周期与线程模型 | `src/isis/isis_main.c`, `src/isis/work/isis_worker.c` |
| IIH 邻居发现 | `src/isis/work/isis_neighbor.c` |
| LSP 发送/接收/LSDB/Flooding | `src/isis/work/isis_lsp.c` |
| SPF 与多路径路由结果 | `src/isis/work/isis_spf.c`, `src/isis/work/isis_route.c` |
| 路由模块同步 | `src/isis/work/isis_route_sync.c` |
| 运行态数据结构 | `src/isis/work/isis_worker.h` |

## 1. 模块结构与执行入口

当前 ISIS 模块不是“收包函数 + 算法函数”的松散集合，而是一个明确的单 worker 线程模型。

### 1.1 生命周期入口

`src/isis/isis_main.c`

- `isis_module_init()` 创建 ISIS 模块 IPC 上下文。
- `isis_on_start()` 连接 CLI、DB、IF、ROUTE 模块。
- `isis_on_ready()` 完成三件事：
  - `isis_db_init()`
  - `isis_worker_prepare()`
  - `isis_worker_launch()`
- 同时通过 `if_api_subscribe_all()` 订阅接口事件，并调用 `isis_db_restore()` 恢复配置。

### 1.2 Worker 线程事件源

`src/isis/work/isis_worker.c`

worker 线程在 `isis_worker_thread_fn()` 中跑 `epoll_wait()`，有 3 类事件源：

- `cmd_eventfd`
  - 处理 CLI show、IF 事件、配置 apply。
- `raw_fd`
  - 由 `isis_neighbor_prepare()` 打开的 `AF_PACKET` 原始套接字。
  - 收到二层 ISIS 帧后进入 `isis_neighbor_handle_raw_event()`。
- `tick_fd`
  - 1 秒周期 `timerfd`。
  - 进入 `isis_neighbor_handle_tick_event()`，负责 hello 定时、LSP 定时和老化。

可以把主循环理解成：

```text
MODULE_READY
  -> worker 启动
    -> epoll 等待
      -> raw_fd  收包
      -> tick_fd 定时发送/老化
      -> cmd_eventfd 处理配置与 show
```

### 1.3 核心运行态数据结构

`src/isis/work/isis_worker.h`

每个 ISIS 实例 `isis_instance_cfg_t` 维护 5 组核心状态：

- `if_cfgs`
  - ISIS 参与接口配置。
- `route_states`
  - 本地直连前缀注入到路由模块的状态。
- `learned_route_heads`
  - 邻居 host 路由和 LSP 学到的 SPF 路由，多路径存储。
- `neighbors`
  - 邻居表，key 是 `ifname|level|sysid`。
- `lsdb_entries`
  - LSDB，key 是 `level|sysid`。

这里已经能看出当前实现的基本建模：

- 邻居状态来自 IIH。
- 拓扑状态来自收到的 LSP。
- 路由结果不是直接挂在 LSDB 上，而是通过 SPF 重算后放进 `learned_route_heads`。

## 2. 报文封装与二层收发模型

### 2.1 二层封装

`src/isis/work/isis_neighbor.c` 和 `src/isis/work/isis_lsp.c`

当前实现走的是广播 LAN 场景，而不是 P2P 场景：

- 原始套接字：`socket(AF_PACKET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, htons(ETH_P_ALL))`
- 收包时按 `802.3 length + LLC` 解析，而不是 Ethertype。
- LLC 固定为：
  - `DSAP = 0xFE`
  - `SSAP = 0xFE`
  - `CTRL = 0x03`
- ISIS NLPID 固定为 `0x83`

Level 对应的目标 MAC：

- L1: `01:80:C2:00:00:14`
- L2: `01:80:C2:00:00:15`

所以实际发出去的帧格式是：

```text
Dst MAC | Src MAC | 802.3 Length | LLC(0xFE 0xFE 0x03) | ISIS PDU
```

收包入口 `isis_neighbor_handle_raw_event()` 会：

1. 过滤本机发出的 `PACKET_OUTGOING`。
2. 过滤非 802.3 length frame。
3. 校验 LLC 三字节。
4. 取出 ISIS PDU。
5. 同时分发给：
   - `isis_handle_iih_payload()`
   - `isis_lsp_handle_pdu()`

也就是说，邻居和 LSDB 不是两套收包口，而是共用一个二层入口。

## 3. IIH 报文格式与邻居发现流程

### 3.1 当前实现发送的是 LAN IIH

`src/isis/work/isis_neighbor.c`

PDU 类型常量：

- L1 LAN IIH: `15`
- L2 LAN IIH: `16`

头长固定：

- `ISIS_LAN_IIH_HDR_LEN = 27`

### 3.2 IIH 编码字段

IIH 在 `isis_build_iih_pdu()` 中构造。按代码对应字段看，大致如下：

| 字段 | 代码行为 |
| --- | --- |
| NLPID | `0x83` |
| Header Length | 固定 `27` |
| PDU Type | `15` 或 `16` |
| Circuit Type | 根据实例 `is_type` 选 `1/2/3` |
| System ID | 从 `inst->net` 中提取 |
| Holding Time | `hello_interval * hold_multiplier` |
| PDU Length | 现场回填 |
| Priority | 固定 `64` |
| LAN ID | `system-id + pseudonode 0` |

后续 TLV 目前会附加：

- `TLV 1` Area Address
- `TLV 129` Protocols Supported NLPIDs
- `TLV 132` IPv4 Interface Address
- `TLV 232` IPv6 Interface Address
- `TLV 6` IS Neighbors

其中有两个实现细节很重要：

1. `TLV 6` 当前发的是空值
   - `isis_tlv_append(... ISIS_TLV_IS_NEIGHBORS, NULL, 0u)`
   - 也就是没有真正把已知邻居列表带出去。
2. IIH 里带哪些 AF，主要看实例 AF 和接口地址缓存
   - 不是完全按接口级 AF 协商结果来编码。
   - 但“这口能不能发 hello”又确实受接口级配置控制。

所以当前 IIH 更接近“带地址的 hello/keepalive”，而不是完整 RFC 三次握手语义。

### 3.3 IIH 发送节拍

发送路径：

- `isis_neighbor_handle_tick_event()`
  - `isis_send_hello_instance()`
    - `isis_send_hello_if_cb()`
      - `isis_send_iih_on_if()`

节拍规则：

- `tick_fd` 每 1 秒触发一次。
- 真正 hello 周期取接口 AF 配置里的 `hello_interval`。
- 用 `if_cfg->last_hello_tx_msec` 控制发送节流。

接口满足以下条件才会发 hello：

- 实例 `admin_up`
- 接口在 `if_cfgs` 中
- 至少一个 AF `enabled && !passive`
- IF cache 中该接口 `proto_up`
- `ifindex != 0`

如果实例启用了 L1/L2，则会分别发 L1 IIH 和 L2 IIH。

### 3.4 IIH 接收与邻居建表

接收路径：

- `isis_neighbor_handle_raw_event()`
  - `isis_handle_iih_payload()`
    - `g_hash_table_foreach(instances, isis_rx_neighbor_apply_instance, &ctx)`

`isis_handle_iih_payload()` 做的事：

1. 根据 `pdu[4]` 判断是 L1 还是 L2 IIH。
2. 从二层 `ifindex` 反查逻辑接口名。
3. 从 PDU 固定头解析：
   - `system_id`
   - `hold_time_sec`
   - `priority`
4. 顺序扫描 TLV，只解析两类地址：
   - `TLV 132` -> 邻居 IPv4 地址
   - `TLV 232` -> 邻居 IPv6 地址
5. 然后把这份邻居上下文投递给每个 ISIS 实例匹配。

### 3.5 “邻居协商”在当前实现里的真实含义

`isis_rx_neighbor_apply_instance()` 是邻居协商的核心。

它不是完整 ISO/ISIS 三路协商，而是一个简化版的“收 hello -> 建邻居 -> 再收一次 hello -> Up”流程：

- 第一次看到某个 `(ifname, level, system-id)`：
  - 新建 `isis_neighbor_t`
  - `state = ISIS_ADJ_STATE_INIT`
  - 记录 hold time、IPv4/IPv6 地址、最后看到时间
- 再次看到同一邻居：
  - `state = ISIS_ADJ_STATE_UP`
  - 更新地址、hold time、last_seen

也就是说，`INIT -> UP` 的推进条件只是“再次收到相同邻居的 IIH”，不是：

- 对端在 hello 里显式回显本端 system-id
- 校验 `TLV 6 IS Neighbors`
- 校验三次握手/three-way adjacency 状态

这和标准 ISIS 的邻接协商有差距，属于当前实现的简化。

### 3.6 邻居可用条件与路由协商条件

邻居记录存在，不等于某个 AF 一定可用于下发路由。

`isis_neighbor_reconcile_learned_afi()` 会按 AF 决定是否生成邻居 host 路由。需要同时满足：

- 实例 `admin_up`
- 实例 AF 已开启
- 本地接口 AF 已开启
- 本地接口 AF 不是 passive
- 接口 `proto_up`
- 邻居 level 与本实例 level 匹配
- 邻居在该 AF 下确实带了地址 TLV

满足时会下发一条主机路由：

- IPv4: `/32`
- IPv6: `/128`
- metric = 本地接口 metric + 固定 `10`

route key 格式为：

```text
host|<ifname>|<level>|<sysid>|<afi>
```

### 3.7 邻居删除条件

邻居老化由 `isis_neighbor_reconcile_instance_now()` 和 `isis_neighbor_should_remove()` 完成。

会删邻居的场景包括：

- hold timer 超时
- 实例 `admin_up` 关闭
- level 不再启用
- 接口不存在/接口 down
- 接口已经没有可发 hello 的 active AF

删除时会顺带做三件事：

1. 删除邻居 host 路由
2. 删除这个邻居相关的 SPF 路由
3. 从 LSDB 中移除该邻居作为 origin 的 LSP

## 4. LSP 报文格式、交换与 LSDB

### 4.1 当前实现发送的是 LSP，不含 CSNP/PSNP

`src/isis/work/isis_lsp.c`

当前代码只显式处理两类 ISIS 数据面 PDU：

- L1 LSP: `18`
- L2 LSP: `20`

没有看到：

- CSNP
- PSNP
- DIS 选举
- pseudonode LSP

所以当前报文交换模型是：

```text
LAN IIH 发现邻居
  -> 周期发送 LSP
  -> 收到更新的 LSP 后写 LSDB
  -> 直接泛洪到其他接口
```

属于“简化 flooding + 本地重算”的实现。

### 4.2 LSP 固定头格式

`isis_lsp_build_pdu()` 构造的固定头可对应为：

| 字段 | 代码行为 |
| --- | --- |
| NLPID | `0x83` |
| Header Length | 固定 `27` |
| PDU Type | `18` 或 `20` |
| PDU Length | 回填 |
| Remaining Lifetime | 固定 `120s` |
| LSP ID | `system-id + pseudonode 0 + fragment 0` |
| Sequence Number | 每 level 独立递增 |
| Checksum | 当前固定写 `0` |
| Type Block | L1 写 `1`，L2 写 `2` |

这说明当前实现的 LSP 模型非常集中：

- 每个实例每个 level 只发 1 个 origin LSP
- 没有分片编号递增
- 没有 pseudonode 扩展
- 没有真正计算 checksum

### 4.3 LSP TLV 组织

当前只组织三类 TLV：

- `TLV 22` Extended IS Reachability
- `TLV 135` Extended IPv4 Reachability
- `TLV 236` IPv6 Reachability

生成来源：

- `isis_lsp_collect_is_reach_cb()`
  - 从邻居表抽取邻接关系，生成 TLV 22。
- `isis_lsp_collect_reach_cb()`
  - 从 IF cache 抽取本地已启用 AF 的接口前缀，生成 TLV 135/236。

### 4.4 TLV 22: 邻接拓扑

`isis_lsp_collect_is_reach_cb()`

每条邻接 entry 编码为 11 字节基本体：

- 邻居 system-id 6 字节
- pseudonode-id 1 字节，当前固定 0
- metric 3 字节
- sub-TLV len 1 字节，当前固定 0

一个重要细节：

- 这里跳过的只是 `state == DOWN`
- `INIT` 和 `UP` 都会被广告进 TLV 22

也就是说，当前实现里邻居第一次收到 hello 进入 `INIT` 后，就已经可能被写入本地 LSP 并参与 SPF，而不是一定等到 `UP`。

### 4.5 TLV 135/236: 前缀可达性

`isis_lsp_append_reach_entry()`

当前前缀 entry 编码形式是：

- 4 字节 metric
- 1 字节控制位，当前固定 0
- 1 字节 prefix length
- N 字节 prefix

生成规则：

- IPv4 从 `if_entry->ipv4_addr/prefix_len` 取
- IPv6 从 `if_entry->ipv6_addr/prefix_len` 取
- metric 来自接口 AF metric，默认 `10`

这里的 passive 语义值得注意：

- passive AF 不参与 hello/LSP 邻接建立
- 但接口 AF 只要 `enabled`，本地 connected prefix 仍可能被放进 reachability TLV

这符合“被动接口仍然可广告本地前缀”的设计思路。

### 4.6 LSP 周期发送

发送路径：

- `isis_neighbor_handle_tick_event()`
  - `isis_lsp_send_due(inst, raw_fd, now_msec)`

发送周期常量：

- `ISIS_LSP_TX_INTERVAL_SEC = 10`

每个实例分别维护：

- `lsp_seq_l1`
- `lsp_seq_l2`
- `last_lsp_tx_msec`

流程是：

1. 到发送周期时，分别构造 L1/L2 PDU。
2. 遍历所有活跃接口，逐口发送。
3. 哪个 level 真正发出去了，就更新对应 `seq`。

### 4.7 LSP 接收与 LSDB 入库

接收路径：

- `isis_neighbor_handle_raw_event()`
  - `isis_lsp_handle_pdu()`
    - `g_hash_table_foreach(instances, isis_lsp_apply_instance_cb, &ctx)`

`isis_lsp_handle_pdu()` 解析：

- level
- ingress ifname
- origin system-id
- lifetime
- checksum
- seq
- TLV 起始位置与长度

`isis_lsp_apply_instance_cb()` 做 5 件关键事：

1. 校验本实例是否启用该 level，接口是否 active。
2. 忽略自己发出的 origin。
3. 检查序列号是否比当前新。
   - 若 `ctx->seq <= last_seq`，通常丢弃。
   - 但如果距离上次接收已超过 `ISIS_LSP_SEQ_RESTART_GRACE_MSEC`，允许对端从较小 seq “重启接管”。
4. 更新 `lsdb_entries`。
5. 触发 SPF，并把该 LSP 向其他接口泛洪。

LSDB key 是：

```text
<level>|<system-id-hex>
```

`isis_lsdb_entry_t` 保存：

- `rx_ifname`
- `level`
- `system_id`
- `lifetime_sec`
- `checksum`
- `seq`
- `ipv4_prefix_count`
- `ipv6_prefix_count`
- `last_rx_msec`
- `tlvs` 原始字节

这说明 LSDB 在本实现里是“最新 LSP 原始 TLV 缓存”，不是已经解出来的拓扑树。

### 4.8 LSP 泛洪

`isis_lsp_flood_instance()`

更新 LSP 入库后会直接 flood：

- 遍历实例的所有活跃接口
- 跳过 ingress 接口
- 对其余接口原样转发收到的 PDU

当前 flooding 的特点：

- 没有 PSNP/CSNP 可靠同步
- 没有 ack 机制
- 没有 DIS/pseudonode
- 是“只要更优/更新就扩散”的 best-effort flood

### 4.9 LSDB 老化

`isis_lsp_reconcile_instance()`

每次邻居 reconcile 时，都会顺便扫 LSDB：

- 计算 `now - last_rx_msec`
- 超过 `lifetime_sec` 就删除 entry
- 删除前调用 `isis_spf_withdraw_origin_routes()` 撤销该 origin 产生的路由

## 5. SPF 拓扑建模与最短路计算

### 5.1 SPF 不是直接在邻居表上算，而是图模型

`src/isis/work/isis_spf.c`

SPF 图节点是 `isis_spf_node_t`：

- `system_id`
- `edges`
- `dist`
- `first_hop`
- `visited`

边是 `isis_spf_edge_t`：

- `to_sysid`
- `metric`

### 5.2 图是怎么建出来的

`isis_spf_recompute_instance()` 每次重算都会：

1. 建一个空图 `nodes`
2. 把本地 system-id 放进去
3. `isis_spf_add_root_edges()`
   - 从邻居表把 “本机 -> 邻居” 的边加进去
4. `isis_spf_collect_graph_from_lsdb()`
   - 从 LSDB 的 `TLV 22` 把 “origin -> remote neighbor” 的边加进去

图边来源分成两段：

```text
本地 system-id
  --(接口 metric)--> 直接邻居
  --(LSP TLV 22)--> 远端拓扑
```

这里有一个实现约束：

- `isis_spf_parse_ext_is_reach_tlv()` 只接受 `neighbor_id[6] == 0`
- 即只处理普通 system-id，不处理 pseudonode

所以当前 SPF 图是“非 DIS、非 pseudonode”的简化模型。

### 5.3 Dijkstra 实现

`isis_spf_run_dijkstra()`

算法是标准的朴素版 Dijkstra：

- `dist` 初始为无穷
- 根节点 `dist = 0`
- 每轮 `isis_spf_pick_next_node()` 选未访问且 dist 最小的点
- 扫描边并松弛

额外维护了 `first_hop`：

- 若当前点就是 root，则子节点的 `first_hop = edge->to_sysid`
- 否则继承父节点 `first_hop`

这使得算法本来具备“从本地根回推首跳”的能力。

但当前前缀选路还有一个更特别的设计，见下文。

## 6. SPF 前缀路由生成流程

### 6.1 关键思想：对每个本地首跳分别跑一次 SPF

这部分是当前实现最重要的设计点。

`isis_spf_collect_prefixes_from_lsdb()` 并不是只从本地 system-id 跑 1 次 SPF 然后直接取结果，而是：

1. 先收集“本地可用首跳” `isis_spf_collect_local_hops()`
2. 对每个首跳 `hop` 单独执行：
   - `isis_spf_run_dijkstra(nodes, hop->system_id)`
3. 再遍历所有 LSDB origin，把它们的前缀挂到这个首跳上

本地首跳 `isis_spf_local_hop_t` 包含：

- 邻居 system-id
- 到该邻居的本地接口 metric
- `out_ifindex`
- `source_addr`
- `nexthop_addr`

也就是说，当前 SPF 不是“先算本机到每个前缀的一条路”，而是“先枚举本机所有可能首跳，再算每个首跳到各 origin 的成本”。

### 6.2 每条前缀 metric 的组成

对某个首跳 `hop`、某个 origin LSP、某个前缀，总代价是：

```text
total_metric
  = local_metric(本机到首跳邻居)
  + dist(hop -> origin)
  + remote_prefix_metric(origin LSP 里的前缀 metric)
```

代码对应：

- `hop->local_metric`
- `isis_spf_get_distance(nodes, entry->system_id, &dist)`
- `remote_metric` 来自 `TLV 135/236`

这正是 `docs/dev/isis-spf-multipath.md` 里所解释的多路径来源。

### 6.3 路由 entry 生成

`isis_spf_parse_prefix_entries()`

把每个前缀转成 `isis_route_state_t`：

- `afi`
- `prefix_addr`
- `prefix_len`
- `out_ifindex`
- `source_addr`
- `nexthop_addr`
- `metric`

然后生成两个 key：

route key:

```text
lsp|<level>|<origin-sysid>|<afi>|<prefix>/<len>
```

path key:

```text
<route_key>|oif=<out_ifindex>|nh=<nexthop>|src=<source>
```

一个 route key 下可以挂多条 path。

### 6.4 多路径存储与最优路径选取

`src/isis/work/isis_route.c`

多路径容器是：

- `isis_route_head_t`
  - 一个前缀的候选路径集合
- `isis_route_path_t`
  - 一条具体路径

`isis_route_head_table_add_path()` 会把路径插入 head，并按下列顺序排序：

1. `metric`
2. `out_ifindex`
3. `nexthop_addr`
4. `source_addr`
5. `path_key`

排序后的第一条就是 best path。

所以当前实现支持：

- 一个前缀保留多条候选路径
- 但只把 best path 下发给 Route 模块

### 6.5 下发到 Route 模块的行为

`isis_route_reconcile_spf()`

SPF 重算不会直接粗暴全删全加，而是做一次 best-path 对账：

1. 从 `desired_heads` 取每个前缀的 best。
2. 和 `inst->learned_route_heads` 当前 best 比较。
3. 对变化的 best：
   - 先 `isis_route_sync_publish_del()`
   - 再 `isis_route_sync_publish_add()`
4. 最后把完整的新 `head + path_list` 拷贝回 `inst->learned_route_heads`

也就是说：

- 内存里保留多路径
- 对 Route 模块只发布 best

`isis_route_sync_publish_add()/del()` 最终通过 `route_rpc_add_wait()/del_wait()` 同步到 Route 模块。

### 6.6 SPF 触发时机

当前有 3 类主要触发点：

1. 收到 LSP
   - `isis_lsp_apply_instance_cb()`
   - 调 `isis_spf_process_lsp()`
2. 邻居/接口状态变化
   - `isis_neighbor_reconcile_instance_now()`
   - 末尾调 `isis_spf_reconcile_instance()`
3. LSDB 老化或邻居删除
   - 删除 origin 后撤路再重算

需要注意的是，`isis_spf_process_lsp()` 当前并没有做增量更新，它直接忽略传入参数，调用：

```c
isis_spf_recompute_instance(inst);
```

因此当前模型是“每次关键变化都整实例全量 SPF 重算”。

## 7. 报文交换到路由收敛的完整时序

把上面的代码拼起来，当前 ISIS 的主流程可以写成：

```text
1. MODULE_READY
   -> worker 启动
   -> raw socket + timerfd 就绪

2. 定时器每秒触发
   -> isis_send_hello_instance()
   -> isis_lsp_send_due()
   -> isis_neighbor_reconcile_all()

3. 本端发 LAN IIH
   -> 对端收到后写入 neighbors(state=INIT/UP)
   -> 可立即生成邻居 host 路由

4. 本端周期发 LSP
   -> TLV 22 带邻接
   -> TLV 135/236 带本地前缀

5. 对端收 LSP
   -> 校验 seq 是否更新
   -> 写入 lsdb_entries[level|sysid]
   -> 触发 SPF 全量重算
   -> 泛洪到其他活跃接口

6. SPF 重算
   -> 建图(本地邻居边 + LSDB TLV22 边)
   -> 对每个本地首跳分别跑 Dijkstra
   -> 解析各 origin 的前缀 TLV
   -> 形成每前缀的多条 path
   -> 选 best path

7. Route 对账
   -> best 变化则 add/del Route 模块
   -> non-best 保留在 ISIS 内存态
```

## 8. 当前实现与标准 ISIS 的差异/边界

如果后续要继续做协议增强，这一节最值得看。

### 8.1 已实现的核心能力

- LAN IIH 邻居发现
- L1/L2 LSP 周期发送
- 基于 seq 的“更新优先”接收
- 基于 raw TLV 的 LSDB 保存
- LSP 泛洪
- 基于 TLV 22/135/236 的 SPF 路由重算
- IPv4/IPv6 双栈路由学习
- 多路径内存保留，best-path 对外发布

### 8.2 明确未实现或做了简化的点

1. 没有 CSNP/PSNP
   - LSDB 同步不是可靠有确认的，而是靠更新 LSP + flood。

2. 没有 DIS / pseudonode
   - LSP 中 pseudonode-id 固定 0。
   - SPF 也只接受普通 system-id 邻接。

3. 邻居协商不是标准 three-way adjacency
   - `TLV 6 IS Neighbors` 发送为空，接收侧也不据此判断。
   - `INIT -> UP` 仅由“再次收到相同 IIH”推进。

4. `INIT` 邻居已经可被拿来做广告和 SPF
   - TLV 22、root edge、local hop 收集都只排除 `DOWN`。
   - 这意味着路由学习早于严格意义上的 fully-up adjacency。

5. LSP checksum 当前没有真正实现
   - 发送侧固定写 0。
   - 接收侧只保存，不校验。

6. 每个 `(level, system-id)` 只保留 1 条最新 LSP
   - 没有多 fragment LSDB。

7. SPF 是整实例全量重算
   - 没有做增量 SPF。

## 9. 结合现有 show 命令的排查建议

如果要边看代码边验证运行态，建议按这个顺序看：

1. `show isis neighbor <tag> verbose`
   - 看邻居是 `Init` 还是 `Up`
   - 看 IPv4/IPv6 地址是否被对端 hello 带上来

2. `show isis ipv4 lsdb <tag>`
3. `show isis ipv6 lsdb <tag>`
   - 看 LSP 是否入库，seq/lifetime 是否刷新

4. `show isis ipv4 route <tag>`
5. `show isis ipv6 route <tag>`
   - 看 SPF 结果与 best path 是否切换

仓库里现成的 ISIS CI 例子也能辅助理解：

- `scripts/ci/modules/isis/n2-l1-g1/`
- `scripts/ci/modules/isis/n4-l4-g12/`

## 10. 结论

当前仓库里的 ISIS 可以概括成一句话：

> 用 LAN IIH 建立一个简化邻居表，用 LSP 携带邻接与前缀广告，把收到的最新 LSP 原样放进 LSDB，再通过“按本地首跳分别跑 SPF”的方式生成多条候选路径，最后只把最优路径发布给 Route 模块。

如果从协议完整度看，它还不是完整 RFC 意义上的 ISIS 实现；但从工程链路看，已经具备了一条清晰的可运行闭环：

```text
接口事件/hello -> 邻居表 -> LSP/LSDB -> SPF -> Route 发布
```

后续如果要继续增强，优先级通常会落在：

- three-way adjacency
- CSNP/PSNP
- pseudonode/DIS
- LSP checksum
- 增量 SPF
