# LLDP 进度

LLDP 已从实现计划推进到可构建、可配置、可通过 raw socket 收发的独立模块。当前状态说明见 `docs/dev/lldp-implementation-plan.md`，用户 CLI 见 `docs/cli/lldp.md`。

## 已完成

- 公共集成：
  - `include/dev.h`
  - `include/lldp.h`
  - `src/CMakeLists.txt`
- 模块进程和生命周期：
  - `src/lldp/lldp_proc.c`
  - `src/lldp/lldp_main.c`
  - `src/lldp/resources/module.conf`
  - build target `netnexus-lldp`
- DB/BDR/CLI：
  - `lldp_protocol`
  - `lldp_interface`
  - `lldp`, `no lldp`
  - 全局 timer/hold 命令
  - 接口 enable/admin-status/port-description 命令
  - current-configuration 输出
- 报文编解码：
  - 构建 LLDPDU
  - 解析 LLDPDU
  - 必选 TLV 校验
  - 有界字符串拷贝
  - 跳过未知 TLV
- Worker：
  - epoll 循环
  - timerfd
  - command eventfd
  - 非阻塞 `AF_PACKET` raw socket
  - 周期 TX
  - RX 解析和更新
  - TTL 0 删除
  - 邻居老化
  - 关闭时 best-effort 发送 TTL 0
- 展示和统计：
  - `show lldp`
  - `show lldp interface`
  - `show lldp neighbors`
  - `show lldp neighbors detail`
  - `show lldp statistics`
- CI 资源：
  - `scripts/ci/modules/lldp/n2-l1-g1/top.yaml`
  - `scripts/ci/modules/lldp/n2-l1-g1/lldp_basic.py`

## 已验证

此前已在本地验证：

```bash
cmake --build build-codex -j2
python3 -m py_compile scripts/ci/modules/lldp/n2-l1-g1/lldp_basic.py
```

## 待验证

在具备权限的 Docker/GNS3 CI 环境中运行 LLDP 拓扑场景：

```bash
./scripts/dev/build-docker-image.sh --docker-image netnexus-ci:localtest
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/lldp/n2-l1-g1 \
  --report-dir scripts/ci/reports/lldp
```

该场景要求 `netnexus-lldp` 具备 raw socket 权限。

## 当前设计决策

- LLDP 保持为 `src/lldp/` 下的独立模块。
- LLDP 在 worker 中直接使用 raw `AF_PACKET` 以太网 socket。
- LLDP 依赖 IF cache 获取逻辑接口名、物理接口名、ifindex 和链路状态。
- 全局 `lldp` admin-up 默认启用符合条件的物理以太接口。
- 接口 `no lldp enable` 会作为显式关闭 override 持久化；`lldp enable` 恢复隐式默认并删除该 override。
- 邻居状态仅运行时保存，不持久化。
