# NetNexus 开发指南

本文档记录当前代码形态下的本地开发流程。NetNexus 现在是一个 DEV supervisor + 多模块独立进程的系统，不再是单体 telnet server，也不是共享库插件式模块。

## 快速开始

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh
```

另开终端连接本地 console：

```bash
./build/bin/netnexus-console
```

默认管理入口是 Unix socket console。telnet/vty 需要通过 ACCESS 命令显式开启。

## 构建

```bash
./scripts/dev/build.sh              # Debug 构建
./scripts/dev/build.sh --release    # Release 构建
./scripts/dev/build.sh --clean      # 清理后重建
./scripts/dev/build.sh -j 8         # 指定并行数
```

等价 CMake 流程：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
```

构建产物：

```text
build/bin/netnexus          # DEV supervisor 主进程
build/bin/netnexus-console  # console 客户端
build/bin/netnexus-*        # 各模块独立进程
build/lib/                  # utils/dev/模块 API 共享库
```

## 运行和调试

```bash
./scripts/dev/start.sh      # 正常启动
./scripts/dev/debug.sh      # gdb 启动 supervisor
```

`start.sh` 会：

- 保持 cwd 在项目根目录，使默认数据落到 `data/`。
- 不设置 `NN_WORK_DIR`，资源文件从 `src/*/resources` 自动发现。
- 尽力加载 `mpls_router`、`mpls_iptunnel`、`mpls_gso`。
- 设置 `LD_LIBRARY_PATH=build/lib`。
- 放开 core dump 限制。

常用清理：

```bash
./scripts/dev/clean.sh
./scripts/dev/clean.sh --data
./scripts/dev/clean.sh --all
```

## 运行时目录

开发模式未设置 `NN_WORK_DIR`：

```text
src/<module>/resources/     # commands.xml/module.conf
data/                       # SQLite、配置快照、core
build/bin/                  # 可执行文件
build/lib/                  # 共享库
```

部署模式设置 `NN_WORK_DIR=/opt/netnexus`：

```text
/opt/netnexus/bin/
/opt/netnexus/lib/
/opt/netnexus/resources/<module>/
/opt/netnexus/data/
/opt/netnexus/log/
/opt/netnexus/run/console.sock
```

## 模块模型

当前模块以独立进程方式构建。新增模块时至少需要：

1. `src/<module>/CMakeLists.txt`，生成 `netnexus-<module>`。
2. `src/<module>/<module>_proc.c` 作为进程入口。
3. `src/<module>/<module>_main.c` 或等价 init/cleanup 逻辑。
4. `src/<module>/resources/module.conf`，供 supervisor 扫描。
5. `src/<module>/resources/commands.xml`，供 CLI 命令树加载。
6. 必要时在 `include/` 增加跨模块公共头文件。
7. 在 `src/CMakeLists.txt` 添加 `add_subdirectory(<module>)`。

已有模块包括 `access`、`cli`、`db`、`dev`、`if`、`vrf`、`route`、`fib`、`bgp`、`sbmp`、`isis`、`ldp`、`lldp`、`tunnel`。

## CLI 开发

命令树由各模块 `src/<module>/resources/commands.xml` 描述。修改命令后重新构建并启动：

```bash
./scripts/dev/build.sh
./scripts/dev/start.sh
```

运行时可用以下命令检查注册结果：

```text
show cli command-info
show cli context
show current-configuration
show this
show dev modules
show dev ipc <module-name>
```

CLI 文档入口位于 `docs/cli/`。

## 数据库和配置

当前配置数据通过 DB/BDR 机制和 SQLite 持久化。常用 CLI：

```text
save configuration [name]
startup configuration <name> db
startup configuration <name> cfg
show startup configuration
show configuration replay-failures
show current-configuration
```

开发环境数据默认在 `data/`，不要假设存在旧式 `data/bgp/bgp_db.db` 单模块路径。需要查看实际表时，先通过 CLI 查询：

```text
show db table-list
show db table-field <table-name>
show db table-data <table-name>
```

也可以直接检查当前 SQLite 文件：

```bash
find data -name '*.db' -print
```

## Docker

```bash
./scripts/docker/install-docker.sh
./scripts/dev/build-docker-image.sh
```

本地容器测试：

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

## CI 和拓扑测试

CI case 目录形态：

```text
scripts/ci/modules/<module>/<top_case>/
  top.yaml
  *.py
```

构建 CI 镜像并运行：

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

只拉起拓扑：

```bash
scripts/dev/top-up.sh \
  --top scripts/ci/modules/if/n2-l1-g1/top.yaml \
  --image netnexus-ci:localtest
```

更多细节见 `scripts/ci/README.md`。

## 内存和崩溃调试

ASAN：

```bash
./scripts/dev/build-with-asan.sh
./build-asan/bin/netnexus
```

Valgrind：

```bash
./scripts/dev/check-memory-leaks.sh
```

Core dump：

```bash
ulimit -c unlimited
./scripts/dev/start.sh
gdb ./build/bin/netnexus core.*
```

运行中 attach：

```bash
pgrep -af netnexus
sudo gdb -p <pid>
```

## IDE

生成 clangd 编译数据库：

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -sf build/compile_commands.json compile_commands.json
```

格式化：

```bash
./scripts/dev/format-code.sh
```

## 故障排查

构建依赖：

```bash
pkg-config --modversion glib-2.0 libxml-2.0 sqlite3
```

库加载：

```bash
ldd ./build/bin/netnexus
LD_DEBUG=libs ./scripts/dev/start.sh
```

资源文件：

```bash
find src -path '*/resources/commands.xml' -print
find src -path '*/resources/module.conf' -print
```

console socket：

```bash
./build/bin/netnexus-console
docker exec -it -e NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock <container> /opt/netnexus/bin/netnexus-console
```
