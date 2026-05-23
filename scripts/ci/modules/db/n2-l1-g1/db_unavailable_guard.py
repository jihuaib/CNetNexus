#!/usr/bin/env python3
"""
DB 不可用时配置 Guard 行为回归。

覆盖目标：验证业务模块 (VRF/IF/Route/BGP/ISIS/LDP/SBMP/Dev) 在 DB 模块
unavailable 时具备 "all-or-nothing" 语义——直接拒绝配置下发，避免内存/OS
与 DB 出现静默偏移。

流程：
  Phase 1: 应用基线配置 (sysname / loop 接口 + IP / 静态路由 / VRF / BGP / ISIS)
  Phase 2: show current-configuration 验证基线全部落盘
  Phase 3: process stop db -> 等 db=down (show module 状态变 down)
  Phase 4: 在 db down 状态尝试改 sysname / 新增 loop / 新增静态路由 /
           新建 vrf / 改 bgp。期望每条都收到 "DB Error" 或 "*** Error:
           configuration rejected ***" 响应；show current-configuration 与
           Phase 2 完全一致，没有任何 silent apply。
  Phase 5: process start db -> 等 db=up
  Phase 6: 重新下发 Phase 4 同样的配置，期望成功并出现在 show current 中
  Phase 7: process reboot db -> 等 db=up；验证 Phase 6 全部配置仍然存在
           （持久层数据在重启后被业务模块按需读取/由 BDR 已恢复）
  Phase 8: 清理
"""

from __future__ import annotations

import time

from module_api import (  # noqa: E402
    check_output,
    cmd,
    mark_step_failed,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


# ------------------------------------------------------------------
# 基线配置
# ------------------------------------------------------------------
BASE_SYSNAME = "r1-base"
BASE_LOOP_ID = 101
BASE_LOOP_V4 = "10.101.0.1"
BASE_LOOP_V4_PREFIX = 32
BASE_ROUTE_PREFIX = "203.0.113.0"
BASE_ROUTE_MASK = 24
BASE_ROUTE_NH = "192.0.2.10"
BASE_VRF = "guard-base"
BASE_BGP_AS = 65010
BASE_ISIS_TAG = 11
BASE_ISIS_NET = "49.0001.0000.0000.0a02.00"

BASE_CONFIG_COMMANDS = [
    "config",
    f"sysname {BASE_SYSNAME}",
    f"if loop {BASE_LOOP_ID}",
    f"ip address {BASE_LOOP_V4} {BASE_LOOP_V4_PREFIX}",
    "exit",
    f"route static ipv4 {BASE_ROUTE_PREFIX} {BASE_ROUTE_MASK} {BASE_ROUTE_NH}",
    f"vrf {BASE_VRF}",
    "exit",
    f"bgp {BASE_BGP_AS}",
    "exit",
    f"isis {BASE_ISIS_TAG}",
    f"net {BASE_ISIS_NET}",
    "exit",
    "end",
]

BASE_EXPECTED_FRAGMENTS = [
    f"sysname {BASE_SYSNAME}",
    f"if loop {BASE_LOOP_ID}",
    f"ip address {BASE_LOOP_V4} {BASE_LOOP_V4_PREFIX}",
    f"route static ipv4 {BASE_ROUTE_PREFIX} {BASE_ROUTE_MASK} {BASE_ROUTE_NH}",
    f"vrf {BASE_VRF}",
    f"bgp {BASE_BGP_AS}",
    f"isis {BASE_ISIS_TAG}",
    f"net {BASE_ISIS_NET}",
]


# ------------------------------------------------------------------
# DB-down 期间的"应被拒绝"配置（每模块各一条，覆盖各 *_main.c 的 guard）
# ------------------------------------------------------------------
DOWN_SYSNAME = "r1-blocked"
DOWN_LOOP_ID = 202
DOWN_LOOP_V4 = "10.202.0.1"
DOWN_LOOP_V4_PREFIX = 32
DOWN_ROUTE_PREFIX = "198.51.100.0"
DOWN_ROUTE_MASK = 24
DOWN_ROUTE_NH = "192.0.2.20"
DOWN_VRF = "guard-blocked"
DOWN_BGP_RTR_ID = "10.202.0.1"
DOWN_ISIS_TAG_CHANGE = "is-type level-2"

# 每个 step 表达：标签 / 命令序列 / 验证当 db 不可用时应出现的错误关键字
BLOCKED_STEPS: list[dict[str, object]] = [
    {
        "label": "Dev sysname rejected when db down",
        "commands": [
            "end",
            "config",
            f"sysname {DOWN_SYSNAME}",
            "end",
        ],
        # Dev 模块的 guard 关键字
        "expect_error_in": [f"sysname {DOWN_SYSNAME}"],
        "expect_keywords": ["Dev Error", "DB module is not available"],
        # 静默偏移检查：不应出现在 show current-configuration
        "absent_after": [f"sysname {DOWN_SYSNAME}"],
    },
    {
        "label": "IF loop creation rejected when db down",
        "commands": [
            "end",
            "config",
            f"if loop {DOWN_LOOP_ID}",
            "end",
        ],
        "expect_error_in": [f"if loop {DOWN_LOOP_ID}"],
        "expect_keywords": ["IF Error", "DB module is not available"],
        "absent_after": [f"if loop {DOWN_LOOP_ID}"],
    },
    {
        "label": "Route static add rejected when db down",
        "commands": [
            "end",
            "config",
            f"route static ipv4 {DOWN_ROUTE_PREFIX} {DOWN_ROUTE_MASK} {DOWN_ROUTE_NH}",
            "end",
        ],
        "expect_error_in": [
            f"route static ipv4 {DOWN_ROUTE_PREFIX} {DOWN_ROUTE_MASK} {DOWN_ROUTE_NH}"
        ],
        "expect_keywords": ["Route Error", "DB module is not available"],
        "absent_after": [
            f"route static ipv4 {DOWN_ROUTE_PREFIX} {DOWN_ROUTE_MASK} {DOWN_ROUTE_NH}"
        ],
    },
    {
        "label": "VRF creation rejected when db down",
        "commands": [
            "end",
            "config",
            f"vrf {DOWN_VRF}",
            "end",
        ],
        "expect_error_in": [f"vrf {DOWN_VRF}"],
        "expect_keywords": ["VRF Error", "DB module is not available"],
        "absent_after": [f"vrf {DOWN_VRF}"],
    },
    {
        "label": "BGP router-id rejected when db down",
        "commands": [
            "end",
            "config",
            f"bgp {BASE_BGP_AS}",
            f"router-id {DOWN_BGP_RTR_ID}",
            "end",
        ],
        "expect_error_in": [f"router-id {DOWN_BGP_RTR_ID}"],
        "expect_keywords": ["BGP Error", "DB module is not available"],
        "absent_after": [f"router-id {DOWN_BGP_RTR_ID}"],
    },
    {
        "label": "ISIS is-type rejected when db down",
        "commands": [
            "end",
            "config",
            f"isis {BASE_ISIS_TAG}",
            DOWN_ISIS_TAG_CHANGE,
            "end",
        ],
        "expect_error_in": [DOWN_ISIS_TAG_CHANGE],
        "expect_keywords": ["ISIS Error", "DB module is not available"],
        "absent_after": [f"is-type level-2"],
    },
]


# ------------------------------------------------------------------
# 工具函数
# ------------------------------------------------------------------
def _show_current(rt: TopologyRuntime, device: str) -> str:
    """走出 config 视图并 dump show current-configuration"""
    outputs = run_cmds(
        rt=rt,
        device=device,
        strict=False,
        timeout=20,
        commands=["end", "show current-configuration"],
    )
    return outputs[-1] if outputs else ""


def _assert_show_contains(rt: TopologyRuntime, device: str, fragments: list[str], *, label: str) -> None:
    cur = _show_current(rt, device)
    violations = check_output(cur, contains=fragments)
    if violations:
        mark_step_failed(label)
        raise AssertionError(f"{label}: {'; '.join(violations)}\noutput:\n{cur}")


def _assert_show_absent(rt: TopologyRuntime, device: str, fragments: list[str], *, label: str) -> None:
    cur = _show_current(rt, device)
    violations = check_output(cur, not_contains=fragments)
    if violations:
        mark_step_failed(label)
        raise AssertionError(
            f"{label}: configuration leaked into show despite DB down: "
            f"{'; '.join(violations)}\noutput:\n{cur}"
        )


def _wait_db_state(rt: TopologyRuntime, device: str, *, expect_up: bool, timeout: int = 30) -> None:
    """
    通过 `show dev modules` 等 db 模块 IPC 列变为期望状态。

    输出形如：
        Registered Modules:
          ID         Name           Phase        Port   IPC    PID
          ----------------------------------------------------------------
          2          db             READY        4002   up     1234
    """
    deadline = time.time() + timeout
    expected = "up" if expect_up else "down"
    last_out = ""
    while time.time() < deadline:
        out = cmd(rt, device, "show dev modules", strict=False)
        last_out = out
        for line in out.splitlines():
            parts = line.split()
            # 行格式：id name phase port ipc pid
            if len(parts) >= 5 and parts[1] == "db":
                if parts[4] == expected:
                    return
                break
        time.sleep(1)
    mark_step_failed()
    raise AssertionError(
        f"db module IPC state not '{expected}' within {timeout}s; last `show dev modules`:\n{last_out}"
    )


def _run_blocked_step_check_resp(rt: TopologyRuntime, device: str, item: dict[str, object]) -> None:
    """db down 状态下：下发命令，校验每条配置命令回显含明确的拒绝关键字"""
    label = str(item["label"])
    commands = list(item["commands"])  # type: ignore[arg-type]
    expect_keywords = [str(x) for x in item.get("expect_keywords", [])]  # type: ignore[arg-type]

    step(f"Phase 4 [{label}]")
    outputs = run_cmds(rt=rt, device=device, strict=False, commands=commands)

    combined = "\n".join(outputs)
    if expect_keywords and not all(kw in combined for kw in expect_keywords):
        mark_step_failed(label)
        raise AssertionError(
            f"{label}: expected all of {expect_keywords} in response, got:\n{combined}"
        )


def _full_cleanup(rt: TopologyRuntime, device: str) -> None:
    step("Hard cleanup (best-effort, ignore failures)")
    run_cmds(
        rt=rt,
        device=device,
        strict=False,
        timeout=15,
        commands=[
            "end",
            "config",
            f"no isis {BASE_ISIS_TAG}",
            "no bgp",
            f"no vrf {BASE_VRF}",
            f"no vrf {DOWN_VRF}",
            f"no route static ipv4 {BASE_ROUTE_PREFIX} {BASE_ROUTE_MASK} {BASE_ROUTE_NH}",
            f"no route static ipv4 {DOWN_ROUTE_PREFIX} {DOWN_ROUTE_MASK} {DOWN_ROUTE_NH}",
            f"no if loop {BASE_LOOP_ID}",
            f"no if loop {DOWN_LOOP_ID}",
            "no sysname",
            "end",
        ],
    )


# ------------------------------------------------------------------
# 主流程
# ------------------------------------------------------------------
def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    device = "r1"

    try:
        # 起始干净
        _full_cleanup(rt, device)

        # ---- Phase 1: baseline 配置 ----
        step("Phase 1: apply baseline configuration (db up)")
        run_cmds(rt=rt, device=device, strict=True, commands=BASE_CONFIG_COMMANDS)

        # ---- Phase 2: 验证基线落盘 ----
        step("Phase 2: verify baseline shows up in current-configuration")
        wait_check(
            rt,
            device=device,
            command="show current-configuration",
            timeout=15,
            interval=2,
            contains=BASE_EXPECTED_FRAGMENTS,
            label="Phase 2 baseline visible in show current",
        )

        # 记录基线 dump 作为之后对比快照
        baseline_dump = _show_current(rt, device)

        # ---- Phase 3: 停掉 db ----
        step("Phase 3: process stop db")
        out_stop = cmd(rt, device, "process stop db", strict=False)
        if "stop db requested" not in out_stop:
            mark_step_failed()
            raise AssertionError(f"unexpected 'process stop db' output:\n{out_stop}")
        _wait_db_state(rt, device, expect_up=False, timeout=20)

        # ---- Phase 4: db down 时配置应被各模块 guard 拒绝 ----
        # 注：db down 时 `show current-configuration` 由各模块 BDR 查 DB 实现，会因
        # db 不可用而返回空/异常。因此 db down 期间不在 show current 上做断言；只
        # 验证每条配置命令的回显含明确的 "DB Error" 拒绝信息。真正的"是否发生
        # silent apply"判据在 Phase 5 db 重新拉起后 show current 验证 (DB 持久层
        # 不应包含任何被拒命令)。
        for blocked in BLOCKED_STEPS:
            _run_blocked_step_check_resp(rt, device, blocked)

        # ---- Phase 5: 拉起 db ----
        step("Phase 5: process start db and wait IPC=up")
        out_start = cmd(rt, device, "process start db", strict=False)
        if "start db ok" not in out_start and "already running" not in out_start:
            mark_step_failed()
            raise AssertionError(f"unexpected 'process start db' output:\n{out_start}")
        _wait_db_state(rt, device, expect_up=True, timeout=30)
        # 给业务模块和 db IPC 一点点时间稳定连接
        time.sleep(2)

        # 关键断言：db 重新可用后 show current 应保持与 baseline 完全一致——
        # 任何 Phase 4 被"拒"的配置都不应出现在 DB 持久层（即未发生 silent apply）。
        step("Phase 5: verify DB persistent layer equals baseline (no silent apply happened)")
        _assert_show_contains(rt, device, BASE_EXPECTED_FRAGMENTS, label="Phase 5 baseline preserved")
        all_blocked_fragments: list[str] = []
        for blocked in BLOCKED_STEPS:
            all_blocked_fragments.extend([str(x) for x in blocked.get("absent_after", [])])  # type: ignore[arg-type]
        _assert_show_absent(rt, device, all_blocked_fragments, label="Phase 5 no silent apply leaked into DB")
        _ = baseline_dump  # 保留供调试

        # ---- Phase 6: 再次下发同样的配置，应成功 ----
        step("Phase 6: re-issue blocked configs after db up (should succeed)")
        for blocked in BLOCKED_STEPS:
            label = str(blocked["label"]) + " (now succeed)"
            step(f"Phase 6 [{label}]")
            run_cmds(
                rt=rt,
                device=device,
                strict=False,
                commands=list(blocked["commands"]),  # type: ignore[arg-type]
            )

        expected_after_recover = [
            f"sysname {DOWN_SYSNAME}",
            f"if loop {DOWN_LOOP_ID}",
            f"route static ipv4 {DOWN_ROUTE_PREFIX} {DOWN_ROUTE_MASK} {DOWN_ROUTE_NH}",
            f"vrf {DOWN_VRF}",
            f"router-id {DOWN_BGP_RTR_ID}",
            "is-type level-2",
        ]
        wait_check(
            rt,
            device=device,
            command="show current-configuration",
            timeout=15,
            interval=2,
            contains=expected_after_recover,
            label="Phase 6 previously-blocked configs now applied",
        )

        # ---- Phase 7: reboot db, 业务配置应继续可见（持久层数据未丢） ----
        step("Phase 7: process reboot db and verify business config survives")
        out_reboot = cmd(rt, device, "process reboot db", strict=False)
        if "reboot db requested" not in out_reboot and "spawned" not in out_reboot:
            mark_step_failed()
            raise AssertionError(f"unexpected 'process reboot db' output:\n{out_reboot}")
        # reboot 会让 db 短暂 down 然后自动起来
        _wait_db_state(rt, device, expect_up=True, timeout=30)
        time.sleep(2)

        # 经过 Phase 6 覆盖式配置后，baseline 中的 sysname / isis is-type 已被
        # 替换；构建实际期望存在的最终配置集合。
        survive_fragments = [
            # baseline 中未被 Phase 6 覆盖的部分
            f"if loop {BASE_LOOP_ID}",
            f"ip address {BASE_LOOP_V4} {BASE_LOOP_V4_PREFIX}",
            f"route static ipv4 {BASE_ROUTE_PREFIX} {BASE_ROUTE_MASK} {BASE_ROUTE_NH}",
            f"vrf {BASE_VRF}",
            f"bgp {BASE_BGP_AS}",
            f"isis {BASE_ISIS_TAG}",
            f"net {BASE_ISIS_NET}",
            # Phase 6 已成功落盘的覆盖/新增部分
            *expected_after_recover,
        ]
        wait_check(
            rt,
            device=device,
            command="show current-configuration",
            timeout=20,
            interval=2,
            contains=survive_fragments,
            label="Phase 7 baseline + Phase 6 configs survive db reboot",
        )

        print("DB unavailable guard check passed.")
    finally:
        _full_cleanup(rt, device)
