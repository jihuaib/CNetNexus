# NetNexus

NetNexus 是一个用 C 实现的模块化网络控制平面实验系统。当前实现采用 DEV supervisor + 多模块独立进程的架构，提供本地 console CLI、配置持久化、接口/VRF/路由/FIB 管理，以及 BGP、BMP Server、ISIS、OSPFv2、OSPFv3、LDP、LLDP、Tunnel 等协议或转发相关模块。

项目主要面向本地开发、Docker/GNS3 拓扑验证和协议功能实验。

## 当前能力

- 多进程模块框架：`netnexus` 负责扫描模块配置、拉起子进程、维护模块生命周期、IPC、订阅和重启。
- 本地 console CLI：默认通过 Unix socket 连接，不依赖 telnet/vty；telnet/vty 需要在配置中显式开启。
- XML 驱动命令树：各模块在 `src/<module>/resources/commands.xml` 注册 CLI 视图、参数和命令。
- 配置与数据库：SQLite 保存运行数据，支持 `save configuration`、启动配置选择、配置回放失败查询。
- 接口与 VRF：支持 GE/loop 接口配置、IPv4/IPv6 地址、接口启停、VRF 创建、VRF 绑定、RD/RT 和 label 策略配置。
- 路由与 FIB：支持 IPv4/IPv6 静态路由、批量静态路由、路由订阅、下一跳对象、Linux FIB/OS 路由同步和 MPLS label 查询。
- 协议模块：
  - BGP：IPv4/IPv6 unicast、VPNv4、IPv4 labeled、QP AF、邻居、update-group、route refresh、VRF import/export、BMP collector 配置。
  - SBMP：BMP server 监听、client/peer/route 展示。
  - ISIS：实例、AF、接口使能、邻居、LSDB、SPF/路由同步。
  - OSPFv2：进程和接口配置、邻居、Router/Network LSA、区域内 SPF、RIB/FIB 路由同步。
  - OSPFv3：IPv6 链路本地邻接、Router/Network/Link/Intra-Area-Prefix LSA、区域内 SPF、IPv6 RIB/FIB 路由同步。
  - LDP：全局/接口使能、hello/hold 定时器、发现与会话基础处理、状态展示。
  - LLDP：全局/接口使能、timer/hold 配置、接口状态展示。
  - Tunnel：candidate、NHLFE、FTN、ILM、watch、label 等 MPLS tunnel 状态展示。
- 运维辅助：日志、core dump、ASAN/Valgrind 脚本、Docker 镜像、GNS3 entrypoint、拓扑驱动 CI。

## 构建产物

构建后主要二进制位于 `build/bin/`：

| 程序 | 作用 |
| --- | --- |
| `netnexus` | DEV supervisor 主进程 |
| `netnexus-console` | 本地 console 客户端 |
| `netnexus-cli` | CLI 模块进程 |
| `netnexus-db` | DB/配置模块进程 |
| `netnexus-access` | console/telnet/vty 接入模块进程 |
| `netnexus-if` | 接口模块进程 |
| `netnexus-vrf` | VRF 模块进程 |
| `netnexus-route` | RIB/静态路由模块进程 |
| `netnexus-fib` | FIB/OS 路由/MPLS 下发模块进程 |
| `netnexus-bgp` | BGP/BMP collector 模块进程 |
| `netnexus-sbmp` | BMP server 模块进程 |
| `netnexus-isis` | ISIS 模块进程 |
| `netnexus-ospf` | OSPFv2 模块进程 |
| `netnexus-ospfv3` | OSPFv3/IPv6 模块进程 |
| `netnexus-ldp` | LDP 模块进程 |
| `netnexus-lldp` | LLDP 模块进程 |
| `netnexus-tunnel` | MPLS tunnel 模块进程 |

共享库输出到 `build/lib/`，包括 `utils`、`dev`、模块 API 库等。

## 目录结构

```text
.
├── include/                 # 跨模块公共头文件
├── src/
│   ├── main.c               # supervisor 入口
│   ├── access/              # console/telnet/vty 接入
│   ├── cli/                 # CLI 框架、命令树、配置展示
│   ├── db/                  # SQLite 与配置快照
│   ├── dev/                 # 模块管理、IPC、订阅、公共设备能力
│   ├── if/                  # 接口管理
│   ├── vrf/                 # VRF 管理
│   ├── route/               # RIB、静态路由、下一跳
│   ├── fib/                 # FIB、OS 路由、MPLS FIB
│   ├── bgp/                 # BGP、VPNv4、BMP collector
│   ├── sbmp/                # BMP server
│   ├── isis/                # ISIS
│   ├── ospf/                # OSPFv2
│   ├── ospfv3/              # OSPFv3
│   ├── ldp/                 # LDP
│   ├── lldp/                # LLDP
│   ├── tunnel/              # MPLS tunnel
│   └── utils/               # 通用库
├── docs/cli/                # 各模块 CLI 文档
├── docs/dev/                # 开发说明和设计记录
├── docs/prod/               # 部署说明
├── scripts/dev/             # 本地构建、启动、调试脚本
├── scripts/ci/              # Docker 拓扑驱动 CI
├── scripts/prod/            # 打包、发布、生产启动脚本
├── scripts/docker/          # Docker 辅助脚本
├── data/                    # 本地运行数据、SQLite、配置快照、core
└── web/                     # Web 辅助服务
```

## 依赖

Ubuntu/Debian：

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libglib2.0-dev libxml2-dev libsqlite3-dev
```

可选开发工具：

```bash
sudo apt install clang-format clang-tidy gdb valgrind tcpdump iproute2 iputils-ping telnet
```

MPLS 相关功能依赖 Linux 内核 MPLS 模块。开发启动脚本会尽力加载：

```bash
sudo modprobe mpls_router
sudo modprobe mpls_iptunnel
sudo modprobe mpls_gso
```

## 本地开发

推荐使用脚本构建和启动：

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh
```

另开终端连接 console：

```bash
./build/bin/netnexus-console
```

常用构建选项：

```bash
./scripts/dev/build.sh --release
./scripts/dev/build.sh --clean
./scripts/dev/build.sh -j 8
```

也可以直接使用 CMake：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
./scripts/dev/start.sh
```

启动脚本会保持工作目录在项目根目录，使默认数据写入 `data/`，并通过可执行文件路径自动发现 `src/*/resources`。

## CLI 快速示例

```text
<NetNexus> show version
<NetNexus> show dev modules
<NetNexus> config
<NetNexus(config)> if GE-1
<NetNexus(config-if-GE-1)> ip address 10.0.0.1 24
<NetNexus(config-if-GE-1)> no shutdown
<NetNexus(config-if-GE-1)> end
<NetNexus> show if
<NetNexus> show route ipv4
```

更多命令见：

- `docs/cli/cli.md`
- `docs/cli/dev.md`
- `docs/cli/db.md`
- `docs/cli/access.md`
- `docs/cli/if.md`
- `docs/cli/vrf.md`
- `docs/cli/route.md`
- `docs/cli/fib.md`
- `docs/cli/bgp.md`
- `docs/cli/isis.md`
- `docs/cli/ospf.md`
- `docs/cli/ldp.md`
- `docs/cli/lldp.md`
- `docs/cli/sbmp.md`
- `docs/cli/tunnel.md`

## 配置持久化

配置和运行数据库默认位于 `data/`。常用命令：

```text
save configuration [name]
startup configuration <name> db
startup configuration <name> cfg
show startup configuration
show configuration replay-failures
show current-configuration
show configuration difference current-configuration <name>
rollback configuration <name>
```

`db` 模式在冷启动时恢复 SQLite 快照；`cfg` 模式从空 running DB 启动后回放配置文本。
回滚设计和安全边界见 `docs/dev/cfg-startup-rollback.md`。

## Docker 和 GNS3

构建本地 Docker 镜像：

```bash
./scripts/dev/build-docker-image.sh
```

运行容器并进入 console：

```bash
docker run -d --rm --name netnexus-dev \
  --cap-add NET_ADMIN \
  --cap-add NET_RAW \
  --sysctl net.ipv6.conf.all.disable_ipv6=0 \
  --sysctl net.ipv6.conf.default.disable_ipv6=0 \
  netnexus:latest

docker exec -it \
  -e NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock \
  netnexus-dev \
  /opt/netnexus/bin/netnexus-console
```

Dockerfile 提供 `dev`、`debug`、`production` 目标。生产/调试镜像默认工作目录为 `/opt/netnexus`，资源文件在 `/opt/netnexus/resources`，数据卷为 `/opt/netnexus/data`，健康检查使用 `/opt/netnexus/run/console.sock`。

发布和部署细节见 `docs/prod/DEPLOYMENT.md`。

## CI 和拓扑验证

CI 用 Docker 容器拉起多节点拓扑，每个 case 目录包含 `top.yaml` 和若干 Python 检查脚本。

```bash
./scripts/dev/build-docker-image.sh --docker-image netnexus-ci:localtest
scripts/ci/run_all.sh --no-build --image netnexus-ci:localtest
```

运行单个 case：

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/bgp/n2-l1-g1 \
  --report-dir scripts/ci/reports/single-case
```

只拉起拓扑、不执行检查：

```bash
scripts/dev/top-up.sh \
  --top scripts/ci/modules/if/n2-l1-g1/top.yaml \
  --image netnexus-ci:localtest
```

详见 `scripts/ci/README.md`。

## 调试和内存检查

GDB：

```bash
./scripts/dev/debug.sh
```

AddressSanitizer：

```bash
./scripts/dev/build-with-asan.sh
./build-asan/bin/netnexus
```

Valgrind：

```bash
./scripts/dev/check-memory-leaks.sh
```

代码格式化：

```bash
./scripts/dev/format-code.sh
```

## 运行时路径

- 未设置 `NN_WORK_DIR`：开发模式，资源从源码目录自动发现，数据写入项目根目录下的 `data/`。
- 设置 `NN_WORK_DIR=/opt/netnexus`：部署模式，使用 `$NN_WORK_DIR/resources`、`$NN_WORK_DIR/data`、`$NN_WORK_DIR/log`、`$NN_WORK_DIR/run`。
- console socket 默认位于 `$NN_WORK_DIR/run/console.sock`；开发模式下由本地路径自动推导，容器内可用 `NN_CONSOLE_SOCK` 显式指定。

## 参考文档

- 开发指南：`docs/dev/DEVELOPMENT.md`
- 部署指南：`docs/prod/DEPLOYMENT.md`
- GNS3 Server：`docs/dev/gns3-server-setup.md`
- BGP VPNv4/VRF 设计记录：`docs/dev/bgp-vpnv4-vrf-import.md`、`docs/dev/bgp-vpnv4-vrf-export.md`
- ISIS 设计记录：`docs/dev/isis-standard-protocol.md`、`docs/dev/isis-spf-multipath.md`
- OSPFv2 CLI 与实现说明：`docs/cli/ospf.md`
- OSPFv3 CLI 与实现说明：`docs/cli/ospfv3.md`
- LLDP 进度记录：`docs/dev/lldp-progress.md`
