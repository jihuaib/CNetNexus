# OSPFv2 CLI 与实现说明

OSPF 模块（module-id: 16）提供 IPv4 OSPFv2 进程、接口、邻居、LSDB、区域内 SPF 和路由同步。

## 模块集成

- 构建产物：`netnexus-ospf`
- IPC 端口：`4016`
- IPC 类别：`0x0010`
- supervisor 配置：`src/ospf/resources/module.conf`
- CLI 命令树：`src/ospf/resources/commands.xml`
- 持久化表：`ospf_instance`、`ospf_area`、`ospf_interface`
- 按需拉起：创建 `ospf_instance` 配置时启动，删除最后一个进程后退出

OSPF 依赖 DB、IF、Route 和 CLI 模块。协议路由以 `ospf` 发布到进程绑定 VRF 的 IPv4
unicast RIB，管理距离为 110，并继续同步到对应 VRF 的 FIB 和 Linux 路由表。

## 当前功能

- Hello、DBD、LS Request、LS Update 和 LS Ack 报文处理。
- `Down`、`Init`、`2-Way`、`ExStart`、`Exchange`、`Loading`、`Full` 邻居状态。
- `broadcast` 和 `point-to-point` 接口网络类型；broadcast 支持 DR/BDR 选举。
- Router-LSA、Network-LSA 的生成、泛洪、刷新、老化和 LSDB 同步。
- 按区域运行 SPF，计算 IPv4 区域内路由并处理邻接或接口失效后的撤销。
- 配置持久化、启动恢复和 `show current-configuration` 输出。

## 进程命令

| 命令 | 视图 | 说明 |
| --- | --- | --- |
| `ospf <process-id>` | config | 创建或进入 OSPF 进程；范围 1-4294967295 |
| `ospf <process-id> vrf <vrf-name>` | config | 创建或进入指定 VRF 的 OSPF 进程；`vrf` 位于 process-id 之后 |
| `no ospf <process-id>` | config | 删除进程及其接口配置 |
| `router-id <ipv4-address>` | ospf | 设置非零 Router ID |
| `no router-id` | ospf | 清除显式 Router ID，由运行时从本进程已启用的 public 接口中选择最高 IPv4 地址 |
| `area <area-id>` | ospf | 创建 normal area；范围 0-4294967295 |
| `no area <area-id>` | ospf | 删除未被接口引用的 area；仍有接口引用时拒绝 |

## 接口命令

接口命令在 `if` 和 `if-loop` 视图可用。必须先创建对应进程，并先执行 `ospf enable`，再配置其他接口参数。
接口所属 VRF 必须与 OSPF 进程绑定的 VRF 一致；未带 `vrf` 的进程属于 `public`。

| 命令 | 说明 |
| --- | --- |
| `ospf enable <process-id> area <area-id>` | 在接口启用 OSPF；area 范围 0-4294967295，不存在时隐式创建 |
| `no ospf enable <process-id>` | 删除该进程的接口配置 |
| `ospf cost <process-id> <cost>` | 设置 cost，范围 1-65535 |
| `no ospf cost <process-id>` | 恢复默认 cost 10 |
| `ospf hello-interval <process-id> <seconds>` | 设置 Hello 间隔，范围 1-65535 秒 |
| `no ospf hello-interval <process-id>` | 恢复默认值 10 秒 |
| `ospf dead-interval <process-id> <seconds>` | 设置 Dead 间隔，范围 1-4294967295 秒 |
| `no ospf dead-interval <process-id>` | 恢复默认值 40 秒 |
| `ospf priority <process-id> <priority>` | 设置广播网络优先级，范围 0-255 |
| `no ospf priority <process-id>` | 恢复默认值 1 |
| `ospf network-type <process-id> broadcast` | 使用广播网络类型，这是默认值 |
| `ospf network-type <process-id> point-to-point` | 使用点到点网络类型 |
| `no ospf network-type <process-id>` | 恢复广播网络类型 |
| `ospf passive <process-id>` | 宣告接口前缀但不收发 OSPF 报文 |
| `no ospf passive <process-id>` | 取消 passive |

Dead 间隔必须大于 Hello 间隔。修改网络类型、Router ID 或接口状态会重建相关邻接和路由。
`no ospf <process-id>` 会级联删除该进程的全部物理接口和 loopback OSPF 配置、Area、邻接、LSDB 与路由；
不需要先逐接口执行 `no ospf enable`。

## 查看命令

| 命令 | 说明 |
| --- | --- |
| `show ospf summary [<process-id>]` | 显示进程概要 |
| `show ospf interface [<process-id>]` | 显示接口、区域、网络类型和状态 |
| `show ospf neighbor [<process-id>] [verbose]` | 显示邻居及可选同步详情 |
| `show ospf lsdb [<process-id>]` | 显示 Router/Network LSA |
| `show ospf route [<process-id>]` | 显示 OSPF 计算出的路由 |

## 配置示例

以下配置在 `GE-1` 建立区域 0 的点到点邻接，并以 passive loopback 宣告 Router ID：

```text
config
if GE-1
 ip address 10.12.0.1 30
 no shutdown
exit
if loop 11
 ip address 10.255.1.1 32
exit
ospf 100
 router-id 10.255.1.1
 area 0
exit
if GE-1
 ospf enable 100 area 0
 ospf network-type 100 point-to-point
 ospf hello-interval 100 2
 ospf dead-interval 100 8
exit
if loop 11
 ospf enable 100 area 0
 ospf passive 100
exit
end
```

可使用以下命令检查收敛和路由安装：

```text
show ospf neighbor 100 verbose
show ospf lsdb 100
show ospf route 100
show route ipv4
show fib os ipv4
```

## CI 验证

仓库包含 NetNexus 双节点和 NetNexus/FRR 互通场景：

```text
scripts/ci/modules/ospf/n2-l1-g1/
scripts/ci/modules/interop/ospf-frr/n2-l1-g1/
```

双节点场景包含点到点和广播网络两组用例，覆盖 Full 邻接、Router/Network LSA 交换、DR/BDR 选举、passive loopback 路由、RIB/FIB/Linux FIB、双向转发、进程重启后的配置恢复，以及接口 flap 撤销与恢复。FRR 场景覆盖点到点互通和撤销。

先构建 NetNexus 和 FRR 互通镜像：

```bash
docker build --target production -t netnexus-ci:localtest .
docker build -f scripts/ci/images/frr/Dockerfile \
  -t netnexus-frr-ci:localtest scripts/ci/images/frr
```

然后可分别运行：

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/ospf/n2-l1-g1 \
  --report-dir scripts/ci/reports/ospf

CNETNEXUS_FRR_IMAGE=netnexus-frr-ci:localtest \
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/interop/ospf-frr/n2-l1-g1 \
  --report-dir scripts/ci/reports/ospf-frr
```

`CNETNEXUS_FRR_IMAGE` 覆盖互通拓扑中的 FRR 镜像标签；未设置时使用拓扑文件中的 `images.frr`，其默认本地标签为 `netnexus-frr-ci:localtest`。

OSPF 使用 IPv4 raw socket（IP protocol 89），运行环境需要 `CAP_NET_RAW`；路由和接口操作还需要 `CAP_NET_ADMIN`。

## 当前范围与限制

- 本模块支持 OSPFv2/IPv4 的 `public` 和命名 VRF；OSPFv3/IPv6 由独立的
  `netnexus-ospfv3` 模块提供，参见 `docs/cli/ospfv3.md`。
- 多个 OSPF 进程必须使用互不重复的 Router ID 和接口；同一接口不能同时加入两个进程。
- 支持多个 normal area 的独立邻接、Type-1/Type-2 LSDB 与区域内 SPF；不同 Area 之间不互通。
- 不支持 ABR Summary、ASBR Summary、External、NSSA LSA、路由重分发、stub/NSSA area、virtual link、NBMA 和 demand circuit。
- 不支持明文或加密认证、graceful restart、opaque LSA 和 traffic engineering。
- 每个前缀当前只向 Route 模块发布一个最佳下一跳，不提供 OSPF ECMP。
- 同一邻居 Router ID 的并行链路暂不支持，运行时只建立第一条有效邻接。
- broadcast/DR/BDR 已覆盖 NetNexus 双节点场景，尚未覆盖与第三方实现的广播网络互通。
