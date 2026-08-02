#!/usr/bin/env python3
"""
NetNexus <-> FRR ISIS dual-stack interop smoke test.

Topology: r1(NetNexus) --- GE-1 / eth1 --- f1(FRR)

Coverage:
- Same Level-1-2 area on both sides, NetNexus cost-style wide vs FRR metric-style wide
- IIH negotiation and L1+L2 adjacency formation
- LSP exchange: NetNexus learns FRR loopback prefixes (v4+v6) and vice versa
- IPv4/IPv6 loopback ping crosses the ISIS-learned routes both ways
- Disable FRR ISIS: NetNexus drops the adjacency
"""

from __future__ import annotations

import re

from module_api import (  # noqa: E402
    frr_config,
    hold_check,
    require_devices,
    run_cmds,
    should_skip_cleanup,
    step,
    wait_check,
    wait_checks,
)
from top_runner import TopologyRuntime  # noqa: E402


TAG = 1
GE_IF = "GE-1"
FRR_LINUX_IF = "eth1"

# 共用 area 49.0001
NN_NET = "49.0001.0000.0000.0001.00"
FRR_NET = "49.0001.0000.0000.0002.00"

NN_LOOP_ID = 11
NN_LOOP_V4 = "10.255.1.1"
NN_LOOP_V4_LEN = 32
FRR_LOOP_V4 = "10.255.2.2"
FRR_LOOP_V4_LEN = 32

NN_LOOP_V6 = "2001:db8:255:1::1"
NN_LOOP_V6_LEN = 128
FRR_LOOP_V6 = "2001:db8:255:2::2"
FRR_LOOP_V6_LEN = 128

NN_LINK_V4 = "10.12.0.1"
FRR_LINK_V4 = "10.12.0.2"

NN_LOOP_V4_PREFIX = f"{NN_LOOP_V4}/{NN_LOOP_V4_LEN}"
FRR_LOOP_V4_PREFIX = f"{FRR_LOOP_V4}/{FRR_LOOP_V4_LEN}"
NN_LOOP_V6_PREFIX = f"{NN_LOOP_V6}/{NN_LOOP_V6_LEN}"
FRR_LOOP_V6_PREFIX = f"{FRR_LOOP_V6}/{FRR_LOOP_V6_LEN}"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup NetNexus/FRR ISIS interop config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"if {GE_IF}",
            f"no isis enable {TAG}",
            f"no isis ipv6 enable {TAG}",
            "exit",
            f"no if loop {NN_LOOP_ID}",
            f"no isis {TAG}",
            "end",
        ],
    )
    frr_config(
        rt,
        "f1",
        [
            f"interface {FRR_LINUX_IF}",
            f"no ip router isis {TAG}",
            f"no ipv6 router isis {TAG}",
            "exit",
            f"no router isis {TAG}",
        ],
        strict=False,
    )
    rt.exec_cmd("f1", f"ip addr del {FRR_LOOP_V4}/{FRR_LOOP_V4_LEN} dev lo", strict=False)
    rt.exec_cmd("f1", f"ip -6 addr del {FRR_LOOP_V6}/{FRR_LOOP_V6_LEN} dev lo", strict=False)


def _configure_netnexus(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"if loop {NN_LOOP_ID}",
            f"ip address {NN_LOOP_V4} {NN_LOOP_V4_LEN}",
            f"ipv6 address {NN_LOOP_V6} {NN_LOOP_V6_LEN}",
            "exit",
            f"isis {TAG}",
            f"net {NN_NET}",
            "is-type level-1-2",
            "cost-style wide",
            "af ipv4",
            "af ipv6",
            "exit",
            "exit",
            f"if {GE_IF}",
            f"isis enable {TAG}",
            f"isis ipv6 enable {TAG}",
            f"isis hello-interval {TAG} 3",
            f"isis ipv6 hello-interval {TAG} 3",
            f"isis hold-multiplier {TAG} 3",
            f"isis ipv6 hold-multiplier {TAG} 3",
            "exit",
            f"if loop {NN_LOOP_ID}",
            f"isis enable {TAG}",
            f"isis ipv6 enable {TAG}",
            f"isis passive {TAG}",
            f"isis ipv6 passive {TAG}",
            "exit",
            "end",
        ],
    )


def _configure_frr(rt: TopologyRuntime) -> None:
    rt.exec_cmd("f1", "ip link set dev lo up")
    rt.exec_cmd("f1", f"ip addr replace {FRR_LOOP_V4}/{FRR_LOOP_V4_LEN} dev lo")
    rt.exec_cmd("f1", f"ip -6 addr replace {FRR_LOOP_V6}/{FRR_LOOP_V6_LEN} dev lo")
    frr_config(
        rt,
        "f1",
        [
            f"router isis {TAG}",
            f"net {FRR_NET}",
            "is-type level-1-2",
            "metric-style wide",
            "exit",
            f"interface {FRR_LINUX_IF}",
            f"ip router isis {TAG}",
            f"ipv6 router isis {TAG}",
            "isis hello-interval 3",
            "isis hello-multiplier 3",
            "exit",
            "interface lo",
            f"ip router isis {TAG}",
            f"ipv6 router isis {TAG}",
            "isis passive",
            "exit",
        ],
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "f1"))

    try:
        _cleanup(rt)

        step("Ensure GE-1/eth1 baseline connectivity")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show if {GE_IF}",
                    "contains": [
                        f"Interface {GE_IF} Detail:",
                        "State      : UP",
                        f"IPv4 Addr  : {NN_LINK_V4}/30",
                    ],
                    "label": "r1 GE-1 up",
                },
                {
                    "device": "f1",
                    "command": f"ip -4 addr show dev {FRR_LINUX_IF}",
                    "contains": [f"{FRR_LINK_V4}/30"],
                    "label": "f1 eth1 has IPv4 address",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 1 -W 2 {NN_LINK_V4}",
                    "contains": ["1 received"],
                    "label": "f1 can ping r1 link IP",
                },
            ],
            timeout=30,
            interval=2,
        )

        step("Configure ISIS on NetNexus and FRR")
        _configure_netnexus(rt)
        _configure_frr(rt)

        step("Wait ISIS adjacency Up on both sides (L1 + L2)")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis neighbor {TAG}",
                    "contains": ["ISIS Neighbors", GE_IF],
                    "regex": [
                        rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L1\s+\S+\s+Up\s+yes\s+yes\b",
                        rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L2\s+\S+\s+Up\s+yes\s+yes\b",
                    ],
                    "label": "NetNexus sees FRR L1+L2 Up",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show isis neighbor'",
                    "contains": [FRR_LINUX_IF],
                    # FRR 列式输出：sysid IF L State HoldTime SNPA
                    "regex": [
                        rf"(?im)^\s*\S+\s+{re.escape(FRR_LINUX_IF)}\s+1\s+Up\b",
                        rf"(?im)^\s*\S+\s+{re.escape(FRR_LINUX_IF)}\s+2\s+Up\b",
                    ],
                    "label": "FRR sees NetNexus L1+L2 Up",
                },
            ],
            timeout=80,
            interval=2,
        )

        step("Hold adjacency stable across multiple hello intervals")
        hold_check(
            rt,
            device="r1",
            command=f"show isis neighbor {TAG}",
            duration=12,
            interval=3,
            regex=[
                rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L1\s+\S+\s+Up\b",
                rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L2\s+\S+\s+Up\b",
            ],
            label="NetNexus keeps ISIS adjacency stable",
        )

        step("Verify NetNexus learns FRR loopback prefixes (IPv4+IPv6)")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show isis route ipv4 {TAG}",
                    "contains": [f"ISIS ipv4 Routes (tag {TAG})", FRR_LOOP_V4_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 learned FRR v4 loopback via ISIS",
                },
                {
                    "device": "r1",
                    "command": f"show isis route ipv6 {TAG}",
                    "contains": [f"ISIS ipv6 Routes (tag {TAG})", FRR_LOOP_V6_PREFIX],
                    "not_contains": ["(no routes)", "(instance not found)"],
                    "label": "r1 learned FRR v6 loopback via ISIS",
                },
                {
                    "device": "r1",
                    "command": "show route ipv4 proto isis",
                    "contains": [FRR_LOOP_V4_PREFIX],
                    "label": "r1 ipv4 RIB has FRR loopback via ISIS",
                },
                {
                    "device": "r1",
                    "command": "show route ipv6 proto isis",
                    "contains": [FRR_LOOP_V6_PREFIX],
                    "label": "r1 ipv6 RIB has FRR loopback via ISIS",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Verify FRR learns NetNexus loopback prefixes (IPv4+IPv6)")
        wait_checks(
            rt,
            [
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ip route isis'",
                    "contains": [NN_LOOP_V4_PREFIX],
                    "label": "FRR ipv4 RIB has NetNexus loopback via ISIS",
                },
                {
                    "device": "f1",
                    "command": "vtysh -c 'show ipv6 route isis'",
                    "contains": [NN_LOOP_V6_PREFIX],
                    "label": "FRR ipv6 RIB has NetNexus loopback via ISIS",
                },
            ],
            timeout=120,
            interval=3,
        )

        step("Verify loopback-to-loopback forwarding via ISIS-learned routes")
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"ping {FRR_LOOP_V4} -a {NN_LOOP_V4}",
                    "contains": ["0% packet loss"],
                    "not_contains": ["100% packet loss", "Network is unreachable"],
                    "label": "r1 loop -> FRR loop IPv4 ping",
                },
                {
                    "device": "r1",
                    "command": f"ping ipv6 {FRR_LOOP_V6} -a {NN_LOOP_V6}",
                    "contains": ["0% packet loss"],
                    "not_contains": ["100% packet loss", "Network is unreachable"],
                    "label": "r1 loop -> FRR loop IPv6 ping",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 3 -W 2 -I {FRR_LOOP_V4} {NN_LOOP_V4}",
                    "contains": [" 0% packet loss"],
                    "label": "FRR lo -> NetNexus loop IPv4 ping",
                },
                {
                    "device": "f1",
                    "command": f"ping -c 3 -W 2 -I {FRR_LOOP_V6} {NN_LOOP_V6}",
                    "contains": [" 0% packet loss"],
                    "label": "FRR lo -> NetNexus loop IPv6 ping",
                },
            ],
            timeout=30,
            interval=3,
        )

        step("Verify NetNexus LSDB carries wide TLVs and FRR origin")
        wait_check(
            rt,
            device="r1",
            command=f"show isis lsdb ipv4 {TAG}",
            timeout=30,
            interval=2,
            contains=["ISIS LSDB"],
            regex=[
                r"(?im)^\s*TLV\[\d+\]\s*:\s*type=22\b",
                r"(?im)^\s*TLV\[\d+\]\s*:\s*type=135\b",
                r"(?im)^\s*TLV\[\d+\]\s*:\s*type=236\b",
                # 验证新增的 TLV 1/129/132 解析
                r"(?im)^\s*TLV\[\d+\]\s*:\s*type=1\b",
                r"(?im)^\s*TLV\[\d+\]\s*:\s*type=129\b",
                r"(?im)^\s*Area\[\d+\]\s*:\s*49\.0001\b",
                r"(?im)^\s*NLPIDs\s*:.*IPv4\(0xcc\).*IPv6\(0x8e\)",
            ],
            label="r1 LSDB has wide TLVs (22/135/236) + Area + Protocols",
        )

        step("Verify NetNexus 'show isis interface' includes DIS election info")
        wait_check(
            rt,
            device="r1",
            command=f"show isis interface ipv4 {TAG}",
            timeout=10,
            interval=2,
            contains=[GE_IF],
            regex=[
                # GE-1 应该有 DIS 信息行，LAN-ID 形如 xxxx.xxxx.xxxx.<cid>
                rf"(?im)^\s*{re.escape(GE_IF)}\s+L[12]\s+dis=[0-9a-f]{{4}}\.[0-9a-f]{{4}}\.[0-9a-f]{{4}}\.[0-9a-f]{{2}}\b",
            ],
            label="r1 show isis interface includes DIS lan-id",
        )

        step("Disable FRR ISIS and verify NetNexus drops adjacency")
        frr_config(
            rt,
            "f1",
            [
                f"interface {FRR_LINUX_IF}",
                f"no ip router isis {TAG}",
                f"no ipv6 router isis {TAG}",
                "exit",
                f"no router isis {TAG}",
            ],
        )
        wait_check(
            rt,
            device="r1",
            command=f"show isis neighbor {TAG}",
            timeout=60,
            interval=2,
            not_regex=[
                rf"(?im)^\s*{TAG}\s+{re.escape(GE_IF)}\s+L[12]\s+\S+\s+Up\b",
            ],
            label="NetNexus drops adjacency after FRR ISIS shutdown",
        )

        print("NetNexus <-> FRR ISIS dual-stack interop check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
