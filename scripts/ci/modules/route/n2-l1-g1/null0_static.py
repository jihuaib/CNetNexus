#!/usr/bin/env python3
"""
Null0 (blackhole) static route end-to-end check (IPv4).

Goals:
- CLI `?` 候选必须包含 ``null0`` 接口（动态参数联想）。
- `route ipv4 <prefix> <len> interface null0` 配置后能下发到 RIB 和 OS（RTN_BLACKHOLE）。
- `no route ipv4 <prefix> <len> interface null0` 之后 RIB 与 OS 均撤销。
- 打开 BGP `import-route static` 后，null0 黑洞路由能被导入到本地 BGP RIB，
  并通告给对端；路由删除后，本地导入消失、对端撤销。
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    cmd_query_help,
    g_top,
    require_devices,
    run_cmds,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


NULL0_PREFIX_ADDR = "198.51.100.0"
NULL0_MASK = "24"
NULL0_PREFIX = f"{NULL0_PREFIX_ADDR}/{NULL0_MASK}"
NULL0_ADD_CMD = f"route ipv4 {NULL0_PREFIX_ADDR} {NULL0_MASK} interface null0"
NULL0_DEL_CMD = f"no route ipv4 {NULL0_PREFIX_ADDR} {NULL0_MASK} interface null0"


def _wait_os_blackhole(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_present: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    """Wait for a blackhole route entry in the OS route table dump."""
    row_regex = (
        rf"(?im)^\s*main\s+blackhole\s+{re.escape(prefix)}\s+-\s+-\s+static\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4 os",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=(
            f"{device} os blackhole {prefix} "
            f"{'present' if expect_present else 'absent'}"
        ),
    )


def _wait_rib_blackhole(
    rt: TopologyRuntime,
    *,
    device: str,
    prefix: str,
    expect_present: bool,
    timeout: int,
    interval: int = 2,
) -> None:
    """Wait for (or the absence of) a blackhole path in `show route ipv4`."""
    row_regex = (
        rf"(?im)^\s*S\s+{re.escape(prefix)}\s+\S+\s+Null0\s+\d+\s+\d+\s*$"
    )
    wait_check(
        rt,
        device=device,
        command="show route ipv4",
        timeout=timeout,
        interval=interval,
        regex=[row_regex] if expect_present else (),
        not_regex=[row_regex] if not expect_present else (),
        label=(
            f"{device} rib blackhole {prefix} "
            f"{'present' if expect_present else 'absent'}"
        ),
    )


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup null0 static + BGP config")
    run_cmds(
        rt=rt,
        device="r2",
        strict=False,
        commands=[
            "config",
            NULL0_DEL_CMD,
            "no bgp",
            "end",
        ],
    )
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "config",
            "no bgp",
            "end",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))
    r1_peer_ip = str(g_top.r1.GE_1.peer_ip)
    r2_peer_ip = str(g_top.r2.GE_1.peer_ip)

    try:
        _cleanup(rt)

        step("Enter config view on r2 for CLI `?` completion test")
        run_cmds(
            rt=rt,
            device="r2",
            commands=["config"],
        )

        step("Verify CLI `?` candidates include null0 for interface ifname")
        partial = f"route ipv4 {NULL0_PREFIX_ADDR} {NULL0_MASK} interface n"
        help_out = cmd_query_help(rt, "r2", partial)
        if not re.search(r"(?m)^\s*null0\b", help_out):
            raise RuntimeError(
                f"expected `null0` candidate in help output for '{partial}?'\n"
                f"got:\n{help_out}"
            )
        print("CLI help contains null0 candidate.")

        step("Leave config view")
        run_cmds(
            rt=rt,
            device="r2",
            commands=["end"],
        )

        step("Configure null0 static route on r2")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                NULL0_ADD_CMD,
                "end",
            ],
        )

        step("Verify null0 route is installed in RIB (Null0 interface)")
        _wait_rib_blackhole(
            rt,
            device="r2",
            prefix=NULL0_PREFIX,
            expect_present=True,
            timeout=30,
        )
        wait_check(
            rt,
            device="r2",
            command=f"show route ipv4 {NULL0_PREFIX_ADDR} {NULL0_MASK}",
            timeout=10,
            contains=[NULL0_PREFIX, "static(blackhole)", "Null0"],
            label="r2 null0 route detail",
        )

        step("Verify null0 route is installed as blackhole in OS")
        _wait_os_blackhole(
            rt,
            device="r2",
            prefix=NULL0_PREFIX,
            expect_present=True,
            timeout=30,
        )

        step("Bring up BGP with import-route static on r2 and peer on r1")
        run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "config",
                "bgp 65001",
                "router-id 1.1.1.1",
                f"neighbor {r1_peer_ip} as 65002",
                "af ipv4-unicast",
                f"neighbor {r1_peer_ip} enable",
                "exit",
                "end",
            ],
        )
        run_cmds(
            rt=rt,
            device="r2",
            strict=False,
            commands=[
                "config",
                "bgp 65002",
                "router-id 2.2.2.2",
                f"neighbor {r2_peer_ip} as 65001",
                "af ipv4-unicast",
                f"neighbor {r2_peer_ip} enable",
                "import-route static",
                "exit",
                "end",
            ],
        )

        step("Wait for BGP sessions Established")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r1_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r1->r2 ipv4-unicast Established",
                },
                {
                    "device": "r2",
                    "command": "show bgp neighbor af ipv4-unicast",
                    "regex": [
                        rf"(?im)^\s*{re.escape(r2_peer_ip)}\s+\S+\s+\S+\s+Established\s*$"
                    ],
                    "label": "r2->r1 ipv4-unicast Established",
                },
            ],
            timeout=30,
        )

        step("Verify null0 route imported into r2 BGP RIB and advertised to r1")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [NULL0_PREFIX],
                    "label": "r2 null0 route imported into BGP RIB",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "contains": [NULL0_PREFIX],
                    "label": "r1 received null0 route from r2",
                },
            ],
            timeout=30,
        )

        step("Delete null0 static route on r2")
        run_cmds(
            rt=rt,
            device="r2",
            commands=[
                "config",
                NULL0_DEL_CMD,
                "end",
            ],
        )

        step("Verify null0 route withdrawn from RIB and OS on r2")
        _wait_rib_blackhole(
            rt,
            device="r2",
            prefix=NULL0_PREFIX,
            expect_present=False,
            timeout=30,
        )
        _wait_os_blackhole(
            rt,
            device="r2",
            prefix=NULL0_PREFIX,
            expect_present=False,
            timeout=30,
        )

        step("Verify BGP RIB drops the imported null0 route on both peers")
        wait_checks(
            rt,
            [
                {
                    "device": "r2",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [NULL0_PREFIX],
                    "label": "r2 null0 route removed from BGP RIB",
                },
                {
                    "device": "r1",
                    "command": "show bgp route af ipv4-unicast",
                    "not_contains": [NULL0_PREFIX],
                    "label": "r1 null0 route withdrawn",
                },
            ],
            timeout=30,
        )

        print("Null0 static route end-to-end check passed.")
    finally:
        _cleanup(rt)
