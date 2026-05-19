#!/usr/bin/env python3
"""
DB 持久化往返回归测试。

覆盖 IF / route / BGP / BMP / ISIS 中适合用 `show current-configuration`
直接验证的 DB-backed 命令，并把 route/ISIS/BGP 的 IPv6 配置面补齐到一个脚本里。

测试流程：
1. 全量配置后通过 show current-configuration 验证已落地。
2. reboot r1，验证全部配置从 DB 恢复。
3. 逐条删除并验证每个细粒度 setter 的 deleter 路径。
4. 再次 reboot，确认删除同样被持久化，不会“死而复生”。

脚本不要求 BGP 邻居 Established，只验证本端配置与 DB 恢复。
"""

from __future__ import annotations

from module_api import (  # noqa: E402
    check_output,
    g_top,
    reboot_device,
    require_devices,
    run_cmds,
    step,
    wait_check,
)
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"

LOOP_ID = 10
LOOP_V4 = "10.10.10.10"
LOOP_V4_PREFIX = 32
LOOP_V6 = "2001:db8:10::10"
LOOP_V6_PREFIX = 128

ROUTE_V4_PREFIX = "198.51.100.0"
ROUTE_V4_MASK = 24
ROUTE_V4_NH = "192.0.2.1"
ROUTE_V4_METRIC = 7

ROUTE_V6_PREFIX = "2001:db8:100::"
ROUTE_V6_MASK = 64
ROUTE_V6_METRIC = 19

BGP_AS_R1 = 65100
BGP_REMOTE_AS = 65200
BGP_ROUTER_ID = LOOP_V4
BGP_KA = 30
BGP_HOLD = 90
BGP_RETRY = 5
BGP_PEER_V4 = "192.0.2.99"
BGP_PEER_V6 = "2001:db8:ffff::99"
BGP_EBGP_TTL = 5

BMP_NAME = "ci-bmp"
BMP_COLLECTOR_IP = "192.0.2.50"
BMP_COLLECTOR_PORT = 5000
BMP_STATS_INTERVAL = 60
BMP_RECONNECT_INTERVAL = 45

ISIS_TAG = 100
ISIS_NET = "49.0001.0000.0000.0a01.00"
ISIS_V6_METRIC = 77
ISIS_V6_HELLO = 11
ISIS_V6_HOLD = 6


CONFIG_COMMANDS = [
    "config",
    f"if loop {LOOP_ID}",
    f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
    f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
    "exit",
    f"route ipv4 {ROUTE_V4_PREFIX} {ROUTE_V4_MASK} {ROUTE_V4_NH} metric {ROUTE_V4_METRIC}",
    f"route ipv6 {ROUTE_V6_PREFIX} {ROUTE_V6_MASK} interface {GE_IF} metric {ROUTE_V6_METRIC}",
    f"bgp {BGP_AS_R1}",
    f"router-id {BGP_ROUTER_ID}",
    f"timer keepalive {BGP_KA} hold {BGP_HOLD}",
    f"timer connect-retry {BGP_RETRY}",
    f"neighbor {BGP_PEER_V4} as {BGP_REMOTE_AS}",
    f"neighbor {BGP_PEER_V4} source-interface loop{LOOP_ID}",
    f"neighbor {BGP_PEER_V4} ebgp-multihop {BGP_EBGP_TTL}",
    f"neighbor {BGP_PEER_V6} as {BGP_REMOTE_AS}",
    f"neighbor {BGP_PEER_V6} source-interface loop{LOOP_ID}",
    f"neighbor {BGP_PEER_V6} ebgp-multihop {BGP_EBGP_TTL}",
    "af ipv4-unicast",
    f"neighbor {BGP_PEER_V4} enable",
    "import-route static",
    "exit",
    "af ipv6-unicast",
    f"neighbor {BGP_PEER_V6} enable",
    "import-route static",
    "exit",
    f"bmp instance {BMP_NAME}",
    f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
    f"stats-report interval {BMP_STATS_INTERVAL}",
    f"reconnect interval {BMP_RECONNECT_INTERVAL}",
    f"monitor neighbor {BGP_PEER_V4}",
    "exit",
    "exit",
    f"isis {ISIS_TAG}",
    f"net {ISIS_NET}",
    "is-type level-2",
    "cost-style wide",
    "no af ipv4",
    "exit",
    f"if loop {LOOP_ID}",
    f"isis ipv6 enable {ISIS_TAG}",
    f"isis ipv6 metric {ISIS_TAG} {ISIS_V6_METRIC}",
    f"isis ipv6 hello-interval {ISIS_TAG} {ISIS_V6_HELLO}",
    f"isis ipv6 hold-multiplier {ISIS_TAG} {ISIS_V6_HOLD}",
    f"isis ipv6 passive {ISIS_TAG}",
    "end",
]


EXPECTED_FRAGMENTS = [
    f"if loop {LOOP_ID}",
    f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
    f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
    f"route ipv4 {ROUTE_V4_PREFIX} {ROUTE_V4_MASK} {ROUTE_V4_NH} metric {ROUTE_V4_METRIC}",
    f"route ipv6 {ROUTE_V6_PREFIX} {ROUTE_V6_MASK} interface {GE_IF} metric {ROUTE_V6_METRIC}",
    f"bgp {BGP_AS_R1}",
    f"router-id {BGP_ROUTER_ID}",
    f"timer keepalive {BGP_KA} hold {BGP_HOLD}",
    f"timer connect-retry {BGP_RETRY}",
    f"neighbor {BGP_PEER_V4} as {BGP_REMOTE_AS}",
    f"neighbor {BGP_PEER_V4} source-interface loop{LOOP_ID}",
    f"neighbor {BGP_PEER_V4} ebgp-multihop {BGP_EBGP_TTL}",
    f"neighbor {BGP_PEER_V6} as {BGP_REMOTE_AS}",
    f"neighbor {BGP_PEER_V6} source-interface loop{LOOP_ID}",
    f"neighbor {BGP_PEER_V6} ebgp-multihop {BGP_EBGP_TTL}",
    "af ipv4-unicast",
    f"neighbor {BGP_PEER_V4} enable",
    "af ipv6-unicast",
    f"neighbor {BGP_PEER_V6} enable",
    "import-route static",
    f"bmp instance {BMP_NAME}",
    f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}",
    f"stats-report interval {BMP_STATS_INTERVAL}",
    f"reconnect interval {BMP_RECONNECT_INTERVAL}",
    f"monitor neighbor {BGP_PEER_V4}",
    f"isis {ISIS_TAG}",
    f"net {ISIS_NET}",
    "is-type level-2",
    "cost-style wide",
    "no af ipv4",
    f"isis ipv6 enable {ISIS_TAG}",
    f"isis ipv6 metric {ISIS_TAG} {ISIS_V6_METRIC}",
    f"isis ipv6 hello-interval {ISIS_TAG} {ISIS_V6_HELLO}",
    f"isis ipv6 hold-multiplier {ISIS_TAG} {ISIS_V6_HOLD}",
    f"isis ipv6 passive {ISIS_TAG}",
]
EXPECTED_COUNTS = {
    "import-route static": 2,
    "af ipv4-unicast": 1,
    "af ipv6-unicast": 1,
}
FINAL_ABSENT_FRAGMENTS = list(dict.fromkeys(EXPECTED_FRAGMENTS))


DELETE_STEPS: list[dict[str, object]] = [
    {
        "label": "BMP monitor neighbor",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"bmp instance {BMP_NAME}",
            f"no monitor neighbor {BGP_PEER_V4}",
            "end",
        ],
        "not_contains": [f"monitor neighbor {BGP_PEER_V4}"],
    },
    {
        "label": "BMP reconnect interval",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"bmp instance {BMP_NAME}",
            "no reconnect interval",
            "end",
        ],
        "not_contains": [f"reconnect interval {BMP_RECONNECT_INTERVAL}"],
    },
    {
        "label": "BMP stats-report interval",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"bmp instance {BMP_NAME}",
            "no stats-report interval",
            "end",
        ],
        "not_contains": [f"stats-report interval {BMP_STATS_INTERVAL}"],
    },
    {
        "label": "BMP collector",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"bmp instance {BMP_NAME}",
            "no collector",
            "end",
        ],
        "not_contains": [f"collector {BMP_COLLECTOR_IP} port {BMP_COLLECTOR_PORT}"],
    },
    {
        "label": "BMP instance",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no bmp instance {BMP_NAME}",
            "end",
        ],
        "not_contains": [f"bmp instance {BMP_NAME}"],
    },
    {
        "label": "BGP IPv4 AF import-route",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "af ipv4-unicast",
            "no import-route static",
            "end",
        ],
        "count": {"import-route static": 1},
    },
    {
        "label": "BGP IPv4 AF neighbor enable",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "af ipv4-unicast",
            f"no neighbor {BGP_PEER_V4} enable",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V4} enable"],
    },
    {
        "label": "BGP IPv4 AF block",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "no af ipv4-unicast",
            "end",
        ],
        "not_contains": ["af ipv4-unicast"],
    },
    {
        "label": "BGP IPv6 AF import-route",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "af ipv6-unicast",
            "no import-route static",
            "end",
        ],
        "count": {"import-route static": 0},
    },
    {
        "label": "BGP IPv6 AF neighbor enable",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "af ipv6-unicast",
            f"no neighbor {BGP_PEER_V6} enable",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V6} enable"],
    },
    {
        "label": "BGP IPv6 AF block",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "no af ipv6-unicast",
            "end",
        ],
        "not_contains": ["af ipv6-unicast"],
    },
    {
        "label": "BGP IPv6 neighbor ebgp-multihop",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V6} ebgp-multihop",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V6} ebgp-multihop {BGP_EBGP_TTL}"],
    },
    {
        "label": "BGP IPv6 neighbor source-interface",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V6} source-interface",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V6} source-interface loop{LOOP_ID}"],
    },
    {
        "label": "BGP IPv6 neighbor session",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V6}",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V6} as {BGP_REMOTE_AS}"],
    },
    {
        "label": "BGP IPv4 neighbor ebgp-multihop",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V4} ebgp-multihop",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V4} ebgp-multihop {BGP_EBGP_TTL}"],
    },
    {
        "label": "BGP IPv4 neighbor source-interface",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V4} source-interface",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V4} source-interface loop{LOOP_ID}"],
    },
    {
        "label": "BGP IPv4 neighbor session",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            f"no neighbor {BGP_PEER_V4}",
            "end",
        ],
        "not_contains": [f"neighbor {BGP_PEER_V4} as {BGP_REMOTE_AS}"],
    },
    {
        "label": "BGP VRF timers and router-id",
        "commands": [
            "config",
            f"bgp {BGP_AS_R1}",
            "no timer connect-retry",
            "timer keepalive 60 hold 180",
            "no router-id",
            "end",
        ],
        "not_contains": [
            f"timer keepalive {BGP_KA} hold {BGP_HOLD}",
            f"timer connect-retry {BGP_RETRY}",
            f"router-id {BGP_ROUTER_ID}",
        ],
    },
    {
        "label": "BGP protocol",
        "commands": [
            "config",
            "no bgp",
            "end",
        ],
        "not_contains": [f"bgp {BGP_AS_R1}"],
    },
    {
        "label": "ISIS loop IPv6 passive",
        "commands": [
            "config",
            f"if loop {LOOP_ID}",
            f"no isis ipv6 passive {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis ipv6 passive {ISIS_TAG}"],
    },
    {
        "label": "ISIS loop IPv6 hold-multiplier",
        "commands": [
            "config",
            f"if loop {LOOP_ID}",
            f"no isis ipv6 hold-multiplier {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis ipv6 hold-multiplier {ISIS_TAG} {ISIS_V6_HOLD}"],
    },
    {
        "label": "ISIS loop IPv6 hello-interval",
        "commands": [
            "config",
            f"if loop {LOOP_ID}",
            f"no isis ipv6 hello-interval {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis ipv6 hello-interval {ISIS_TAG} {ISIS_V6_HELLO}"],
    },
    {
        "label": "ISIS loop IPv6 metric",
        "commands": [
            "config",
            f"if loop {LOOP_ID}",
            f"no isis ipv6 metric {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis ipv6 metric {ISIS_TAG} {ISIS_V6_METRIC}"],
    },
    {
        "label": "ISIS loop IPv6 enable",
        "commands": [
            "config",
            f"if loop {LOOP_ID}",
            f"no isis ipv6 enable {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis ipv6 enable {ISIS_TAG}"],
    },
    {
        "label": "ISIS instance IPv4 AF restore",
        "commands": [
            "config",
            f"isis {ISIS_TAG}",
            "af ipv4",
            "end",
        ],
        "not_contains": ["no af ipv4"],
    },
    {
        "label": "ISIS instance is-type",
        "commands": [
            "config",
            f"isis {ISIS_TAG}",
            "is-type level-1-2",
            "end",
        ],
        "not_contains": ["is-type level-2"],
    },
    {
        "label": "ISIS instance",
        "commands": [
            "config",
            f"no isis {ISIS_TAG}",
            "end",
        ],
        "not_contains": [f"isis {ISIS_TAG}", f"net {ISIS_NET}"],
    },
    {
        "label": "Route IPv6 static",
        "commands": [
            "config",
            f"no route ipv6 {ROUTE_V6_PREFIX} {ROUTE_V6_MASK} interface {GE_IF}",
            "end",
        ],
        "not_contains": [f"route ipv6 {ROUTE_V6_PREFIX} {ROUTE_V6_MASK} interface {GE_IF} metric {ROUTE_V6_METRIC}"],
    },
    {
        "label": "Route IPv4 static",
        "commands": [
            "config",
            f"no route ipv4 {ROUTE_V4_PREFIX} {ROUTE_V4_MASK} {ROUTE_V4_NH}",
            "end",
        ],
        "not_contains": [f"route ipv4 {ROUTE_V4_PREFIX} {ROUTE_V4_MASK} {ROUTE_V4_NH} metric {ROUTE_V4_METRIC}"],
    },
    {
        "label": "Loop interface",
        "commands": [
            "config",
            f"no if loop {LOOP_ID}",
            "end",
        ],
        "not_contains": [
            f"if loop {LOOP_ID}",
            f"ip address {LOOP_V4} {LOOP_V4_PREFIX}",
            f"ipv6 address {LOOP_V6} {LOOP_V6_PREFIX}",
        ],
    },
]


def _show_current(rt: TopologyRuntime, device: str) -> str:
    out = run_cmds(
        rt=rt,
        device=device,
        strict=False,
        timeout=20,
        commands=["end", "show current-configuration"],
    )
    return out[-1] if out else ""


def _assert_output(
    label: str,
    text: str,
    *,
    contains: list[str] | None = None,
    not_contains: list[str] | None = None,
    count: dict[str, int] | None = None,
) -> None:
    violations = check_output(
        text,
        contains=contains or [],
        not_contains=not_contains or [],
        count=count or {},
    )
    if violations:
        raise RuntimeError(f"{label} violations: {'; '.join(violations)}\noutput:\n{text}")


def _hard_cleanup(rt: TopologyRuntime) -> None:
    step("Hard cleanup (best-effort, ignore failures)")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"no route ipv6 {ROUTE_V6_PREFIX} {ROUTE_V6_MASK} interface {GE_IF}",
            f"no route ipv4 {ROUTE_V4_PREFIX} {ROUTE_V4_MASK} {ROUTE_V4_NH}",
            f"bgp {BGP_AS_R1}",
            f"bmp instance {BMP_NAME}",
            f"no monitor neighbor {BGP_PEER_V4}",
            "no reconnect interval",
            "no stats-report interval",
            "no collector",
            "exit",
            f"no bmp instance {BMP_NAME}",
            "exit",
            "no bgp",
            f"no isis {ISIS_TAG}",
            f"no if loop {LOOP_ID}",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))
    _ = g_top

    try:
        _hard_cleanup(rt)

        step("Phase A: apply full DB-touching configuration on r1")
        run_cmds(rt=rt, device="r1", strict=True, commands=CONFIG_COMMANDS)

        step("Phase A: verify show current-configuration reflects everything")
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=20,
            interval=2,
            contains=EXPECTED_FRAGMENTS,
            count=EXPECTED_COUNTS,
            label="Phase A current-config contains all DB-backed config",
        )

        step("Phase B: reboot r1 and verify config persisted via DB restore")
        reboot_device(rt, "r1", timeout=120)
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=60,
            interval=3,
            contains=EXPECTED_FRAGMENTS,
            count=EXPECTED_COUNTS,
            label="Phase B current-config restored from DB after reboot",
        )

        step("Phase C: delete config one step at a time and verify each disappears")
        for delete_step in DELETE_STEPS:
            label = str(delete_step["label"])
            step(f"Phase C [{label}]")
            run_cmds(
                rt=rt,
                device="r1",
                strict=False,
                commands=list(delete_step["commands"]),
            )
            current = _show_current(rt, "r1")
            _assert_output(
                f"after delete: {label}",
                current,
                not_contains=list(delete_step.get("not_contains", [])),
                count=dict(delete_step.get("count", {})),
            )

        step("Phase C: final current-configuration should not contain any test fragment")
        final = _show_current(rt, "r1")
        _assert_output(
            "Phase C final cleanup",
            final,
            not_contains=FINAL_ABSENT_FRAGMENTS,
            count={"import-route static": 0},
        )

        step("Phase D: reboot r1 again to confirm deletes are persisted (no resurrection)")
        reboot_device(rt, "r1", timeout=120)
        wait_check(
            rt,
            device="r1",
            command="show current-configuration",
            timeout=60,
            interval=3,
            not_contains=FINAL_ABSENT_FRAGMENTS,
            count={"import-route static": 0},
            label="Phase D no resurrection after second reboot",
        )

        print("DB persist + reboot + delete cycle passed.")
    finally:
        _hard_cleanup(rt)
