# OSPFv3 CLI 与实现说明

OSPFv3 模块（module-id: 17）按照 RFC 5340 实现 `public` 和命名 VRF 内的 IPv6 区域内路由。
模块使用 IPv6 链路本地地址建立邻接，以 IPv4 格式的 32 位 Router ID 标识路由器。

## 模块集成

- 构建产物：`netnexus-ospfv3`
- IPC 端口：`4017`
- IPC 类别：`0x0011`
- supervisor 配置：`src/ospfv3/resources/module.conf`
- CLI 命令树：`src/ospfv3/resources/commands.xml`
- 持久化表：`ospfv3_instance`、`ospfv3_area`、`ospfv3_interface`
- 按需拉起：创建首个 OSPFv3 进程时启动，删除最后一个进程后退出

OSPFv3 依赖 DB、IF、Route 和 CLI 模块。计算出的 IPv6 路由以 `ospfv3` 协议发布到
Route，管理距离为 110，并继续同步到 FIB 和 Linux IPv6 路由表。

## 当前功能

- IPv6 raw socket、链路本地源地址、`ff02::5`/`ff02::6` 组播和 IPv6 伪首部校验和。
- Hello、DBD、LS Request、LS Update 和 LS Ack 报文处理。
- `Down`、`Init`、`2-Way`、`ExStart`、`Exchange`、`Loading`、`Full` 邻居状态。
- `broadcast` 和 `point-to-point` 网络类型；broadcast 支持 DR/BDR 选举。
- Router-LSA、Network-LSA、Link-LSA 和 Intra-Area-Prefix-LSA 的生成、泛洪、刷新、
  老化和 LSDB 同步。
- 按区域运行 SPF，安装 IPv6 区域内路由，并在邻接或接口失效后撤销。
- 配置持久化、启动恢复、进程重启恢复和 `show current-configuration` 输出。

## 进程命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ospfv3 <process-id>` | config | 创建或进入进程；范围 1-4294967295 |
| `ospfv3 <process-id> vrf <vrf-name>` | config | 创建或进入指定 VRF 的进程；`vrf` 位于 process-id 之后 |
| `no ospfv3 <process-id>` | config | 删除进程及其接口配置 |
| `router-id <ipv4-address>` | ospfv3 | 设置非零 32 位 Router ID |
| `no router-id` | ospfv3 | 清除显式 Router ID，运行时从接口地址自动选择 |
| `area <area-id>` | ospfv3 | 创建 normal area；范围 0-4294967295 |
| `no area <area-id>` | ospfv3 | 删除未被接口引用的 area |

## 接口命令

接口命令在 `if` 和 `if-loop` 视图可用。必须先创建对应进程；除 `enable` 外的参数
要求接口已经在该进程中启用。接口所属 VRF 必须与进程绑定的 VRF 一致；未带 `vrf`
的进程属于 `public`。

| 命令 | 说明 |
| --- | --- |
| `ospfv3 enable <process-id> area <area-id>` | 在接口启用 OSPFv3；不存在的 area 会被隐式创建 |
| `no ospfv3 enable <process-id>` | 删除该进程的接口配置 |
| `ospfv3 cost <process-id> <cost>` | 设置 cost，范围 1-65535，默认 10 |
| `ospfv3 hello-interval <process-id> <seconds>` | 设置 Hello 间隔，范围 1-65535，默认 10 |
| `ospfv3 dead-interval <process-id> <seconds>` | 设置 Dead 间隔，范围 1-65535，默认 40 |
| `ospfv3 priority <process-id> <priority>` | 设置广播网络优先级，范围 0-255，默认 1 |
| `ospfv3 network-type <process-id> broadcast` | 使用广播网络类型，这是默认值 |
| `ospfv3 network-type <process-id> point-to-point` | 使用点到点网络类型 |
| `ospfv3 passive <process-id>` | 宣告接口 IPv6 前缀但不收发 OSPFv3 报文 |

相应的 `no ospfv3 <parameter> <process-id>` 命令恢复默认参数，`no ospfv3 passive
<process-id>` 取消 passive。Dead 间隔必须大于 Hello 间隔。

## 查看命令

| 命令 | 说明 |
| --- | --- |
| `show ospfv3 summary [<process-id>]` | 显示进程概要 |
| `show ospfv3 interface [<process-id>]` | 显示接口、区域、网络类型和状态 |
| `show ospfv3 neighbor [<process-id>] [verbose]` | 显示邻居、链路本地地址和同步详情 |
| `show ospfv3 lsdb [<process-id>]` | 显示四类基础 LSA |
| `show ospfv3 route [<process-id>]` | 显示 SPF 计算出的 IPv6 路由 |

## 配置示例

```text
config
if GE-1
 ipv6 address 2001:db8:12::1 64
 no shutdown
exit
if loop 61
 ipv6 address 2001:db8:1::1 128
exit
ospfv3 300
 router-id 10.255.1.1
 area 0
exit
if GE-1
 ospfv3 enable 300 area 0
 ospfv3 network-type 300 point-to-point
 ospfv3 hello-interval 300 2
 ospfv3 dead-interval 300 8
exit
if loop 61
 ospfv3 enable 300 area 0
 ospfv3 passive 300
exit
end
```

检查邻接、LSDB 和路由：

```text
show ospfv3 neighbor 300 verbose
show ospfv3 lsdb 300
show ospfv3 route 300
show route ipv6
show fib os ipv6
```

## CI 验证

双节点基础用例位于 `scripts/ci/modules/ospfv3/n2-l1-g1/`，FRR 互通用例位于
`scripts/ci/modules/interop/ospfv3-frr/n2-l1-g1/`。两者覆盖 Full 邻接、三类点到点
所需 LSA（Router、Link、Intra-Area-Prefix）、passive loopback 路由、RIB/FIB/Linux
FIB、双向 IPv6 转发、进程重启后的恢复，以及与 FRR 的报文和 LSDB 互操作：

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/ospfv3 \
  --report-dir scripts/ci/reports/ospfv3

CNETNEXUS_FRR_IMAGE=netnexus-frr-ci:localtest \
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/interop/ospfv3-frr \
  --report-dir scripts/ci/reports/ospfv3-frr
```

OSPFv3 使用 IP protocol 89 的 IPv6 raw socket，运行环境需要 `CAP_NET_RAW`；路由和接口
操作还需要 `CAP_NET_ADMIN`。

## 当前范围与限制

- 支持 `public` 和命名 VRF 的 IPv6 unicast 与 normal area 内部路由。
- 不支持 ABR/ASBR、Inter-Area-Prefix、Inter-Area-Router、External、NSSA、路由重分发、
  stub/NSSA area、virtual link 和 NBMA。
- 不支持认证、IPsec 集成、graceful restart、扩展 LSA 和 traffic engineering。
- 每个前缀当前只发布一个最佳下一跳，不提供 ECMP。
- 同一邻居 Router ID 的并行链路暂不支持。
