# LLDP 实现说明

本文档记录当前 LLDP 实现状态和设计边界。原始分阶段计划已经落地，历史进度见 `docs/dev/lldp-progress.md`。

## 当前实现

LLDP 已作为独立模块实现：

```text
include/lldp.h
src/lldp/
  lldp_proc.c
  lldp_main.c
  lldp_cli.c
  lldp_db.c
  lldp_bdr.c
  db/
  work/
  resources/module.conf
  resources/commands.xml
```

公共集成点：

- 模块 ID：`14`
- IPC 端口：`4014`
- IPC 类别：`0x000E`
- supervisor 通过 `src/lldp/resources/module.conf` 发现模块。
- CLI 通过 `src/lldp/resources/commands.xml` 加载命令。
- 构建产物为 `netnexus-lldp`。

## 协议范围

当前实现覆盖标准 LLDP 邻居发现：

- 目的 MAC：`01:80:c2:00:00:0e`
- Ethertype：`0x88cc`
- TLV 头部：2 字节，高 7 位为类型，低 9 位为长度
- TX/RX 通过 worker 内的 `AF_PACKET` raw socket 完成
- 邻居 key：`ifname + chassis_id + port_id`
- TTL 0 立即删除邻居
- 定时器周期清理过期邻居

已处理 TLV：

- Chassis ID
- Port ID
- TTL
- 端口描述
- 系统名称
- 系统描述
- 系统能力
- 管理地址
- End of LLDPDU

未知 TLV 会被跳过；LLDP-MED、DCBX、Org-Specific TLV 解释和 SNMP/MIB 不在当前实现范围内。

## 配置模型

持久化表：

```text
lldp_protocol
lldp_interface
```

运行时邻居表只保存在 worker 内存中，不写入 DB。

全局 `lldp` admin-up 后，IF cache 中符合条件的物理以太接口会成为 LLDP 候选接口。接口隐式默认是 enable；
`no lldp enable` 是持久化的 negative override，`lldp enable` 则恢复隐式默认并删除该 override。

## CLI

全局配置：

```text
lldp
no lldp
lldp timer <5-32768>
no lldp timer
lldp hold-multiplier <2-10>
no lldp hold-multiplier
```

接口配置：

```text
lldp enable
no lldp enable
lldp admin-status txrx|rxonly|txonly|disabled
no lldp admin-status
lldp port-description <text>
no lldp port-description
```

展示命令：

```text
show lldp
show lldp interface
show lldp neighbors
show lldp neighbors detail
show lldp statistics
```

面向用户的 CLI 文档见 `docs/cli/lldp.md`。

## 工作线程设计

worker 当前负责：

- epoll 事件循环
- command eventfd
- 1-second timerfd
- 非阻塞 `AF_PACKET` raw socket
- TX 周期调度
- RX 报文解析
- 邻居更新、删除和老化
- 统计计数器

TX 条件：

- 全局 LLDP admin-up
- 接口启用并 link-up
- 接口存在有效 ifindex
- admin-status 允许 TX

RX 条件：

- 收到 EtherType `0x88cc`
- 忽略 `PACKET_OUTGOING`
- `sll_ifindex` 能映射到 IF cache 中的逻辑接口
- 接口启用且 admin-status 允许 RX

模块关闭时会 best-effort 发送 TTL 0 LLDPDU。

## 运行要求

LLDP raw socket 需要 `CAP_NET_RAW`。本地 CMake post-build 会尝试对 `netnexus-lldp` 设置能力；容器/GNS3 环境需要相应 capability。

Docker/GNS3 运行建议：

```text
--cap-add NET_ADMIN --cap-add NET_RAW
```

## 验证

已存在 LLDP CI 场景：

```text
scripts/ci/modules/lldp/n2-l1-g1/top.yaml
scripts/ci/modules/lldp/n2-l1-g1/lldp_basic.py
```

运行方式：

```bash
./scripts/dev/build-docker-image.sh --docker-image netnexus-ci:localtest
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/lldp/n2-l1-g1 \
  --report-dir scripts/ci/reports/lldp
```

## 已知边界

- 不解释 LLDP-MED/DCBX/Org-Specific TLV。
- 不提供 per-TLV enable knob。
- 不持久化邻居状态。
- worker command path 仍以当前模块内部同步分发为主，未抽象成通用队列框架。
