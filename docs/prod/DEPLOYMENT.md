# NetNexus 部署指南

本文档描述当前 NetNexus 的部署方式。当前实现推荐使用 Docker/GNS3 镜像，也支持用 `scripts/prod/package.sh` 生成 `/opt/netnexus` 目录结构的传统安装包。

## 运行要求

Ubuntu/Debian 构建依赖：

```bash
sudo apt update
sudo apt install build-essential cmake pkg-config libglib2.0-dev libxml2-dev libsqlite3-dev
```

运行依赖：

```bash
sudo apt install libglib2.0-0 libxml2 libsqlite3-0 iproute2 iputils-ping tcpdump telnet
```

MPLS 转发相关功能需要宿主机内核模块：

```bash
sudo modprobe mpls_router
sudo modprobe mpls_iptunnel
sudo modprobe mpls_gso
```

容器运行建议带上：

```text
--cap-add NET_ADMIN
--cap-add NET_RAW
--sysctl net.ipv6.conf.all.disable_ipv6=0
--sysctl net.ipv6.conf.default.disable_ipv6=0
```

## 本地构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

或使用脚本：

```bash
./scripts/dev/build.sh --release
```

构建后的 supervisor 为 `build/bin/netnexus`，console 客户端为 `build/bin/netnexus-console`，各模块为 `build/bin/netnexus-*`。

## Docker 部署

构建本地生产镜像：

```bash
./scripts/dev/build-docker-image.sh
```

运行：

```bash
docker volume create netnexus-data

docker run -d \
  --name netnexus \
  --cap-add NET_ADMIN \
  --cap-add NET_RAW \
  --sysctl net.ipv6.conf.all.disable_ipv6=0 \
  --sysctl net.ipv6.conf.default.disable_ipv6=0 \
  -v netnexus-data:/opt/netnexus/data \
  --restart unless-stopped \
  netnexus:latest
```

连接 console：

```bash
docker exec -it \
  -e NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock \
  netnexus \
  /opt/netnexus/bin/netnexus-console
```

健康检查使用 `/opt/netnexus/run/console.sock`，不依赖 telnet。telnet/vty 默认关闭，需要 CLI 配置开启。

## Docker 镜像目标

Dockerfile 当前提供：

| 目标 | 说明 |
| --- | --- |
| `dev` | 开发基础镜像，含构建和调试工具 |
| `debug` | 可部署调试镜像，含 gdb/tcpdump |
| `production` | 默认生产镜像 |

示例：

```bash
docker build --target production -t netnexus:latest .
docker build --target debug --build-arg BUILD_TYPE=Debug -t netnexus:debug .
docker build --target dev -t netnexus:dev .
```

## 发布 Docker 包

生成发布包：

```bash
./scripts/prod/publish.sh          # amd64 + arm64
./scripts/prod/publish.sh amd64    # 仅 amd64
```

加载发布镜像：

```bash
docker load < package/netnexus-1.0.0-docker-amd64.tar.gz
```

发布到 GitHub Release：

```bash
mkdir -p .secrets
chmod 700 .secrets
printf '%s\n' '<YOUR_GITHUB_TOKEN>' > .secrets/github_token
chmod 600 .secrets/github_token
./scripts/prod/publish.sh --github-release
```

多架构构建前置：

```bash
docker run --privileged --rm tonistiigi/binfmt --install all
docker buildx create --name netnexus-builder --driver docker-container --driver-opt network=host --use
docker buildx inspect --bootstrap
```

## 传统安装包部署

生成安装包：

```bash
./scripts/dev/build.sh --release
VERSION=1.0.0 ./scripts/prod/package.sh
```

安装：

```bash
cd package
tar xzf netnexus-1.0.0.tar.gz
cd netnexus-1.0.0
sudo ./scripts/deploy.sh
```

启动：

```bash
sudo /opt/netnexus/scripts/start.sh
```

连接 console：

```bash
sudo NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock /opt/netnexus/bin/netnexus-console
```

当前 `deploy.sh` 安装 `/opt/netnexus/{bin,lib,resources,scripts,data}`，并尝试配置 MPLS 内核模块开机加载。它不创建 systemd unit；如需守护运行，可自行创建 systemd service 调用 `/opt/netnexus/scripts/start.sh`。

## 运行时目录

```text
/opt/netnexus/
├── bin/                 # netnexus、netnexus-console、netnexus-* 模块进程
├── lib/                 # 共享库
├── resources/<module>/  # commands.xml, module.conf
├── scripts/             # start/supervise/gns3/swap-image/deploy
├── data/                # SQLite 和配置快照
├── log/                 # 设置 NN_WORK_DIR 后的模块日志
└── run/console.sock     # 本地 console socket
```

核心环境变量：

| 变量 | 说明 |
| --- | --- |
| `NN_WORK_DIR` | 工作目录；部署镜像默认为 `/opt/netnexus` |
| `NN_CONSOLE_SOCK` | console socket 路径 |
| `LD_LIBRARY_PATH` | 共享库搜索路径 |

## 配置管理

常用 CLI：

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

`db` 模式恢复 SQLite 快照；`cfg` 模式从空 running DB 启动后回放配置文本。

资源文件位于：

```text
/opt/netnexus/resources/<module>/commands.xml
/opt/netnexus/resources/<module>/module.conf
```

修改资源文件后需要重启 NetNexus。

## 升级

Docker：

```bash
./scripts/dev/build-docker-image.sh
docker stop netnexus
docker rm netnexus
# 使用同一个 netnexus-data 数据卷重新运行 docker run
```

传统安装：

```bash
sudo tar czf netnexus-backup-$(date +%Y%m%d).tar.gz /opt/netnexus
VERSION=1.1.0 ./scripts/prod/package.sh
cd package
tar xzf netnexus-1.1.0.tar.gz
cd netnexus-1.1.0
sudo ./scripts/deploy.sh
sudo /opt/netnexus/scripts/start.sh
```

## 故障排查

检查容器：

```bash
docker ps -a | grep netnexus
docker logs netnexus
docker exec netnexus env | grep NN_
docker exec netnexus ls -la /opt/netnexus/resources
docker exec netnexus test -S /opt/netnexus/run/console.sock
```

检查传统安装：

```bash
ls -l /opt/netnexus/bin/netnexus
ls -la /opt/netnexus/resources/*/commands.xml
ldd /opt/netnexus/bin/netnexus
```

检查 MPLS：

```bash
lsmod | grep mpls
sudo modprobe mpls_router mpls_iptunnel mpls_gso
```

检查数据：

```bash
find /opt/netnexus/data -maxdepth 3 -type f -print
```
