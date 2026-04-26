# ISIS 标准协议流程说明

本文只描述标准 ISIS 协议流程，不结合本项目代码实现，重点包括：

1. 邻居协商过程
2. 报文类型与报文格式
3. LSP/LSDB 交换过程
4. SPF 计算过程

## 1. ISIS 基本概念

ISIS 是链路状态型 IGP，直接运行在二层之上，不承载于 IP。

核心概念包括：

- `System-ID`
  - 路由器在 ISIS 域内的唯一标识。
- `NET`
  - Network Entity Title，包含 `Area Address + System-ID + NSEL`。
- `Level-1`
  - 面向 Area 内部的路由。
- `Level-2`
  - 面向 Area 之间骨干的路由。
- `LSDB`
  - 链路状态数据库，保存收到和本地产生的 LSP。
- `SPF`
  - 基于 LSDB 运行 Dijkstra，生成最短路径树。

## 2. ISIS 标准报文类型

ISIS 标准交互主要有 4 类报文：

### 2.1 IIH

`IIH` 用于发现和维持邻居。

按链路类型分为：

- `LAN IIH`
- `P2P IIH`

按级别分为：

- `Level-1 IIH`
- `Level-2 IIH`

主要作用：

- 邻居发现
- 参数协商
- 邻接保活
- 广播网络中的 `DIS` 选举

### 2.2 LSP

`LSP` 用于发布链路状态信息。

主要承载：

- 本路由器的邻接关系
- 可达前缀
- metric
- 标志位
- 可选扩展信息

### 2.3 CSNP

`CSNP` 是 Complete Sequence Number PDU。

作用是通告“我当前 LSDB 中有哪些 LSP”，用于数据库同步检查。

### 2.4 PSNP

`PSNP` 是 Partial Sequence Number PDU。

作用包括：

- 请求缺失的 LSP
- 对收到的 LSP 进行确认

## 3. ISIS 标准报文格式

ISIS 报文通常可以理解为：

```text
公共头 + 各 PDU 固定字段 + TLV
```

### 3.1 公共头

不同 PDU 都带有 ISIS 公共头，常见字段包括：

- `NLPID`
- `Header Length`
- `Version`
- `ID Length`
- `PDU Type`
- `Version2`
- `Max Area Addresses`

### 3.2 IIH 报文格式

IIH 的固定字段通常包括：

- `Circuit Type`
- `Source ID`
- `Holding Time`
- `PDU Length`
- `Priority`，LAN IIH 使用
- `LAN ID`，LAN IIH 使用
- `Local Circuit ID`，P2P IIH 使用

IIH 常见 TLV 包括：

- `Area Address`
- `IS Neighbors`
- `Authentication`
- `Protocols Supported`
- `IPv4 Interface Address`
- `IPv6 Interface Address`
- `Padding`
- `Restart`
- `Three-way Adjacency`

其中最关键的是：

- `Area Address`
  - 用于 Level-1 同 Area 校验。
- `IS Neighbors`
  - 用于判断是否已经形成双向邻接。
- `Authentication`
  - 用于认证校验。

### 3.3 LSP 报文格式

LSP 的固定字段通常包括：

- `Remaining Lifetime`
- `LSP ID`
  - 由 `System-ID + Pseudonode-ID + Fragment Number` 组成
- `Sequence Number`
- `Checksum`
- `Type/Flags`

其中 `Type/Flags` 常见含义包括：

- `L1/L2`
- `ATT`
- `OL`

LSP 中常见 TLV 包括：

- `Area Address`
- `IS Reachability`
- `IPv4 Reachability`
- `IPv6 Reachability`
- `Authentication`
- `Hostname`
- 其他扩展 TLV

### 3.4 CSNP / PSNP 报文格式

CSNP 和 PSNP 的固定字段通常包括：

- `Source ID`
- `PDU Length`
- `Start LSP ID`
- `End LSP ID`

它们携带的核心 TLV 是 `LSP Entry TLV`，每个条目一般包括：

- `LSP ID`
- `Sequence Number`
- `Checksum`
- `Remaining Lifetime`

## 4. 标准邻居协商过程

### 4.1 广播 LAN 场景

广播网络上的标准协商过程如下：

1. 路由器周期发送 `LAN IIH`。
2. 对端收到 IIH 后，检查：
   - `Level` 是否匹配
   - `Area Address` 是否匹配
     - `Level-1` 必须同 Area
     - `Level-2` 不要求同 Area
   - `Authentication` 是否通过
   - 报文是否合法
3. 如果只是单向看见对方，但对方 IIH 中还没有回显自己，则邻接一般处于 `Init`。
4. 当本端在对端 IIH 的 `IS Neighbors TLV` 中看到了自己，说明形成双向邻接，邻接进入 `Up`。
5. 广播网络中，`Up` 后还会进行 `DIS` 选举。
6. `DIS` 当选后负责协助该 LAN 的数据库同步。

### 4.1.1 DIS 选举

DIS 主要用于广播多接入网络。

选举依据通常是：

1. `Priority` 高者优先
2. 若相同，则比 `SNPA/MAC`

DIS 的作用包括：

- 在 LAN 上周期发送 `CSNP`
- 生成 `pseudonode LSP`
- 简化多接入网络的拓扑表示

### 4.2 点到点 P2P 场景

点到点链路上的标准协商过程更简单：

1. 双方周期发送 `P2P IIH`
2. 检查 `Level / Area / Authentication`
3. 完成双向确认后进入 `Up`
4. P2P 不需要 `DIS`
5. 也不需要 LAN 式 pseudonode 表示

## 5. 标准 LSP 交换与 LSDB 同步过程

当邻接建立后，ISIS 开始同步链路状态数据库。

### 5.1 LSP 产生

每台路由器会生成自己的 `LSP`，内容包括：

- 本机邻接
- 本机发布的前缀
- metric
- 标志位

### 5.2 LSP 泛洪

1. 本地生成或更新 LSP
2. 向同级别邻居泛洪
3. 邻居收到后比较：
   - `LSP ID`
   - `Sequence Number`
   - `Checksum`
   - `Remaining Lifetime`
4. 如果是更新版本，则写入本地 `LSDB`
5. 然后继续向其他邻居泛洪

### 5.3 CSNP 同步

在广播 LAN 中，`DIS` 周期发送 `CSNP`。

CSNP 的作用是告诉邻居：

- 我当前数据库有哪些 LSP
- 每个 LSP 的序列号和校验和是什么

邻居据此判断：

- 自己是否缺少某条 LSP
- 自己是否持有旧版本 LSP

### 5.4 PSNP 请求与确认

如果发现缺失或版本旧：

1. 邻居发送 `PSNP`
2. 请求对方重发特定 LSP
3. 对方重发对应 LSP

在一些场景下，PSNP 也承担收到 LSP 后的确认作用。

### 5.5 LSP 老化与刷新

LSP 不是永久有效的。

标准行为包括：

- LSP 带有 `Remaining Lifetime`
- 本地会周期刷新自己的 LSP
- 若某条 LSP 到期未刷新，则从 LSDB 中删除
- LSDB 变化后会触发 SPF 重算

## 6. SPF 计算过程

ISIS 的 SPF 是以 LSDB 为输入的最短路径计算。

### 6.1 每个 Level 独立计算

ISIS 按级别维护独立数据库：

- `Level-1 LSDB`
- `Level-2 LSDB`

SPF 也是按级别分别运行的。

### 6.2 构建拓扑图

SPF 计算前，路由器先从 LSDB 中提取：

- 各节点
- 各链路
- 各链路 metric

然后构成一个有权图。

在广播网络中，如果存在 `pseudonode LSP`，它也会作为图中的节点参与计算。

### 6.3 运行 Dijkstra

1. 以本路由器自己作为根节点
2. 对所有节点初始化距离
3. 反复选择当前最小代价节点
4. 对邻接边做松弛
5. 最终得到到各节点的最短路径树

这一步的结果是：

- 到每个节点的最短路径代价
- 到每个节点的第一跳

### 6.4 从节点可达性转成前缀可达性

最短路径树算出后，再读取各节点在 LSP 中通告的前缀。

对每个前缀，通常计算：

```text
前缀总代价 = 到广告节点的最短路径代价 + 该前缀自身广告 metric
```

然后确定：

- `next-hop`
- `outgoing interface`
- `metric`

### 6.5 ECMP

如果多条路径代价相同，则可以形成 `ECMP`。

ISIS 标准允许等价多路径，具体是否安装到 FIB 由实现决定。

### 6.6 SPF 触发条件

以下变化通常会触发 SPF：

- 邻接建立或断开
- LSP 新增
- LSP 更新
- LSP 过期
- metric 变化
- 标志位变化

## 7. 标准时序总结

可以把标准 ISIS 协议过程概括为：

```text
IIH 发现邻居
  -> 双向确认
  -> 广播网络选举 DIS
  -> 交换 LSP
  -> 通过 CSNP/PSNP 完成 LSDB 同步
  -> 基于 LSDB 运行 SPF
  -> 生成路由
```

如果再压缩成一句话：

> ISIS 先通过 IIH 建立邻接，再通过 LSP/CSNP/PSNP 保持全网 LSDB 一致，最后基于 LSDB 运行 SPF 计算最短路径并生成路由。
