# ISIS SPF: Why Single-LSDB Entry Still Produces Non-Best Paths

## 1. Question

你问的是：

> 当前 LSDB 每个 `(level, system-id)` 只存一条最新 LSP，为什么还能算出“非最优路径”？

结论先说：

- **LSDB 是“拓扑/前缀广告数据库”**，不是“路径结果数据库”。
- 非最优路径不是直接“存出来”的，而是 SPF 在同一份 LSDB 图上，针对多个本地首跳分别计算后推导出来的。

---

## 2. LSDB 到底存什么

当前 LSDB key 是：

- `"<level>|<system-id-hex>"`

也就是说，同一个 `(level, origin system-id)` 只保留一份最新 LSP（覆盖更新）。

这份 LSP 里存的是：

- IS 邻接 TLV（拓扑边）
- IPv4/IPv6 reachability TLV（前缀及其远端 metric）
- 以及接收时间、seq、lifetime 等元数据

它表达的是“网络图 + 前缀广告”，不是“从本机出发的所有可行路径列表”。

### 2.1 `show isis ... lsdb` 里的 metric 列要怎么理解

`show isis ... lsdb` 展示的 `IS-Metric / IPv4-Metric / IPv6-Metric` 是摘要视图（便于观察），
不是“某条从本机出发路径”的最终代价。

真正算路时用的是 LSDB entry 里的原始 TLV（raw bytes）：

- TLV 22：拓扑边及其 metric
- TLV 135/236：前缀及其 remote metric

所以你在 LSDB 表里看到“每个 system-id 一行”，仍然足够恢复整张图和前缀 metric。

---

## 3. 多路径是怎么推导出来的

当前 SPF 路径生成逻辑是：

1. 对每个 `level`、每个 AF（IPv4/IPv6），先收集本地可用首跳邻居（`local hops`）。
2. 对每个首跳 `hop`，把它当作该轮 Dijkstra 的起点（root）在同一份 LSDB 图上跑最短路。
3. 遍历所有 origin LSP 里的前缀 TLV，计算该 `hop` 下到该前缀的总代价：
   - `total_metric = local_metric(hop) + dist(hop -> origin) + remote_prefix_metric`
4. 把每个 `(route_key, hop)` 结果都写入同一个 route-head（`head + path`）：
   - `route_key`：同一前缀/同一来源（例如 `lsp|...|afi|prefix/len`）
   - `path_key`：区分不同路径（包含 oif/nh/src 等）
5. 对 `path_list` 排序，首元素就是 best，其余就是非最优/备选。

所以，多路径来源是“**同图多次根切换计算**”，不是“LSDB 存多条同源 LSP”。

### 3.1 伪代码（当前实现）

```text
for level in [L1, L2]:
  hops = collect_local_hops(level, afi)   # 可能多个：R2, R3...
  for hop in hops:
    run_dijkstra(root = hop.system_id)    # 注意：root 不是本机 R1
    for origin_lsp in lsdb[level]:
      dist = shortest(hop -> origin_lsp.system_id)
      for prefix in origin_lsp.reach_tlv(afi):
        total = hop.local_metric + dist + prefix.remote_metric
        add_path(route_key(prefix, origin, level), path_key(hop,oif,nh,src), total)
sort(path_list by metric/oif/nh/src)
best = path_list[0]
```

---

## 4. 菱形拓扑详细算例（结合你现在的 metric）

拓扑（R1 看 R4 前缀）：

- 分支 A：`R1 -> R2 -> R4`
- 分支 B：`R1 -> R3 -> R4`

脚本里的关键 metric（初始）：

- `R1->R2` 本地接口 metric = `10`
- `R1->R3` 本地接口 metric = `40`
- `R2->R4` 链路 metric = `10`
- `R3->R4` 链路 metric = `10`
- `R4` loopback 前缀 remote metric = `10`

目标前缀：`R4 loop`（如 `10.255.4.4/32`、`2001:db8:255:4::4/128`）

### 4.1 对每个 hop 分别跑

对某个 `level`（L1 或 L2）：

1. hop = `R2`
   - `local_metric = 10`
   - `dist(R2 -> R4) = 10`（来自 LSDB 图最短路）
   - `remote_prefix_metric(R4 loop) = 10`
   - `total = 10 + 10 + 10 = 30`

2. hop = `R3`
   - `local_metric = 40`
   - `dist(R3 -> R4) = 10`
   - `remote_prefix_metric = 10`
   - `total = 40 + 10 + 10 = 60`

于是同一个 `route_key` 下会有两条 path：`30` 和 `60`。
排序后 `30` 是 best，`60` 是非最优备选。

### 4.2 metric 切换后为什么会反转

切换后脚本配置：

- `R1->R2` 改成 `80`
- `R1->R3` 改成 `5`

再算一次（其他不变）：

- 经 R2：`80 + 10 + 10 = 100`
- 经 R3：`5 + 10 + 10 = 25`

best 就变成经 R3（25），原经 R2（100）变成非最优备选。

### 4.3 为什么“LSDB 一条记录”不影响上面两条路径

关键点：

- `R4` 在 LSDB 中每个 level 只有一条 LSP 记录，但这条记录里有它的前缀 metric。
- `R2`、`R3` 各自 LSP 记录提供了到 `R4` 的拓扑边 metric。
- SPF 把这些 LSP 组合成图，再针对 hop=R2/hop=R3 分别跑最短路。

也就是说，多路径信息来自“多次计算 + 同图不同 root”，不是来自“多条 R4 LSP 记录”。

---

## 5. 和“下发到路由模块”的关系

当前模型是：

- `learned_route_heads`：保存同一 `route_key` 的多路径（best + non-best）。
- 仅把每个 head 的 **best path** 下发到 route 模块。
- non-best 作为备选保留在 ISIS 内存态，用于展示/切换。

这就是“存多条、下发一条”的行为。

---

## 6. 常见误解对照

### 误解 1：LSDB 只有一条，所以只能算一条路径

- 错。LSDB 一条是“每个 origin 最新 LSP 一条”；它包含的是图信息。
- 只要本地有多个可用首跳，算法就会得到多条候选路径。

### 误解 2：`show isis ... lsdb` 的 metric 列就是最终路由 metric

- 错。最终路由 metric 是 `local + dist + remote` 三段叠加出来的。
- LSDB 表列只是摘要，不等于最终路径代价。

### 误解 3：非最优路径应该出现在 LSDB

- 错。LSDB 不是路径结果表，路由结果应看 ISIS route 视图。

---

## 7. 为什么 LSDB 里看不到“非最优”

因为 LSDB 展示的是“收到的 LSP 条目”，不是 SPF 结果：

- LSDB：谁广告了什么（拓扑/前缀信息）。
- Route（ISIS 路由视图）：根据 LSDB 计算后的路径结果（可包含 best + non-best）。

所以“LSDB 没有非最优路径”是正常设计，不代表 SPF 算不出非最优路径。

---

## 8. 当前实现边界

- 不是做 route 模块侧 ECMP 安装（当前仍以 best 下发为主）。
- 是在 ISIS 内维护候选路径集合，支持 best 切换与路径可见性。
- 邻居 host 路由（`host|...`）通常只有单路径；LSP 前缀路由（`lsp|...`）可形成多路径集合。
