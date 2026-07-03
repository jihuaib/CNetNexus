#!/usr/bin/env python3
"""
BGP EVPN Type-5 VRF export / EVPN RT / advertise evpn route timing regression.

Covers:
- EVPN Type-5 list key is short and queryable.
- EVPN Type-5 detail keeps the non-key fields (ESI/GW/Label).
- ``advertise evpn route`` gates VRF IPv4 unicast export into public EVPN RIB.
- EVPN export RT add/delete rebuilds route attributes while the route remains.
- RT changes while advertise is disabled do not leak routes; enabling advertise later exports with the latest RT.
"""

from __future__ import annotations

import time

from module_api import hold_check, require_devices, run_cmds, should_skip_cleanup, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


VRF_NAME = "red"
R1_AS = 65001
R1_RD = "65001:51"
RD_NOMATCH = "65009:9"

LOOP_ID = 51
LOOP_ADDR = "100.51.1.1"
LOOP_LEN = 32
PFX = f"{LOOP_ADDR}/{LOOP_LEN}"

EVPN_KEY = f"evpn:type=5,rd={R1_RD},ethag=0,prefix={PFX}"
EVPN_RT1 = "65001:510"
EVPN_RT2 = "65001:511"
EVPN_RT3 = "65001:512"
EVPN_RT1_FMT = f"rt:{EVPN_RT1}"
EVPN_RT2_FMT = f"rt:{EVPN_RT2}"
EVPN_RT3_FMT = f"rt:{EVPN_RT3}"


def _cleanup(rt: TopologyRuntime) -> None:
    step("Cleanup EVPN export timing config")
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no if loop {LOOP_ID}",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def _configure_base(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4",
            f"route-distinguisher {R1_RD}",
            "exit",
            "exit",
            f"if loop {LOOP_ID}",
            f"vrf forwarding {VRF_NAME}",
            f"ip address {LOOP_ADDR} {LOOP_LEN}",
            "exit",
            f"bgp {R1_AS}",
            "router-id 1.1.1.1",
            "af evpn",
            "exit",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "import-route connected",
            "exit",
            "exit",
            "end",
        ],
    )
    # VRF/route publication is asynchronous across modules.
    time.sleep(2)


def _set_advertise(rt: TopologyRuntime, enabled: bool) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"bgp {R1_AS}",
            f"vrf {VRF_NAME}",
            "af ipv4-unicast",
            "advertise evpn route" if enabled else "no advertise evpn route",
            "exit",
            "exit",
            "end",
        ],
    )


def _set_evpn_rt(rt: TopologyRuntime, rt_value: str, enabled: bool) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "config",
            f"vrf {VRF_NAME}",
            "af ipv4",
            f"vpn-target {rt_value} export evpn" if enabled else f"no vpn-target {rt_value} export evpn",
            "exit",
            "exit",
            "end",
        ],
    )


def _evpn_absent_check(label: str) -> dict[str, object]:
    return {
        "device": "r1",
        "command": "show bgp route af evpn",
        "not_contains": [EVPN_KEY],
        "label": label,
    }


def _evpn_present_checks(label: str, *, contains_rt: str | None = None, absent_rts: list[str] | None = None) -> list[dict[str, object]]:
    absent_rts = absent_rts or []
    detail_contains = [
        "BGP Route Detail",
        "EVPN Type     : 5 (IP Prefix)",
        f"EVPN RD       : {R1_RD}",
        "EVPN ESI      : 00:00:00:00:00:00:00:00:00:00",
        "EVPN EthTag   : 0",
        f"EVPN Prefix   : {PFX}",
        "EVPN Gateway  : 0.0.0.0",
        "EVPN Label    : 0",
    ]
    if contains_rt:
        detail_contains.append(contains_rt)

    return [
        {
            "device": "r1",
            "command": "show bgp route af evpn",
            "contains": [EVPN_KEY],
            "not_contains": ["esi=", "gw=", "label="],
            "label": f"{label}: short EVPN list key",
        },
        {
            "device": "r1",
            "command": f"show bgp route af evpn {EVPN_KEY}",
            "contains": detail_contains,
            "not_contains": absent_rts,
            "regex": [r"(?im)^\s*Paths\s*:\s*[1-9]\d*\s*$"],
            "label": f"{label}: EVPN short-key detail query",
        },
        {
            "device": "r1",
            "command": f"show bgp route af evpn rd {R1_RD} {EVPN_KEY}",
            "contains": ["BGP Route Detail", f"RD: {R1_RD}", f"EVPN Prefix   : {PFX}"],
            "label": f"{label}: EVPN short-key detail query with matching RD filter",
        },
        {
            "device": "r1",
            "command": f"show bgp route af evpn rd {RD_NOMATCH} {EVPN_KEY}",
            "not_contains": [f"EVPN Prefix   : {PFX}", "Paths          :"],
            "label": f"{label}: EVPN short-key detail query with non-matching RD filter",
        },
    ]


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    try:
        _cleanup(rt)

        step("Base: VRF RD + loop connected route + public EVPN AF + VRF import-route connected, no advertise")
        _configure_base(rt)
        wait_checks(
            rt,
            [
                {
                    "device": "r1",
                    "command": f"show bgp route af ipv4-unicast vrf {VRF_NAME}",
                    "contains": [PFX],
                    "label": "source VRF ipv4-unicast route ready",
                },
                _evpn_absent_check("EVPN route absent before advertise evpn route"),
            ],
            timeout=30,
        )

        step("Enable advertise evpn route before any EVPN RT: route is exported with short key and no RT")
        _set_advertise(rt, True)
        wait_checks(
            rt,
            _evpn_present_checks(
                "advertise-before-rt",
                absent_rts=[EVPN_RT1_FMT, EVPN_RT2_FMT, EVPN_RT3_FMT],
            ),
            timeout=30,
        )

        step("Add EVPN export RT while advertise is active: route stays and Ext-Comm gains RT1")
        _set_evpn_rt(rt, EVPN_RT1, True)
        wait_checks(
            rt,
            _evpn_present_checks(
                "add-rt1-active",
                contains_rt=EVPN_RT1_FMT,
                absent_rts=[EVPN_RT2_FMT, EVPN_RT3_FMT],
            ),
            timeout=30,
        )

        step("Delete EVPN export RT while advertise is active: route stays and RT1 is removed")
        _set_evpn_rt(rt, EVPN_RT1, False)
        wait_checks(
            rt,
            _evpn_present_checks(
                "delete-rt1-active",
                absent_rts=[EVPN_RT1_FMT, EVPN_RT2_FMT, EVPN_RT3_FMT],
            ),
            timeout=30,
        )

        step("Add a new EVPN export RT while advertise is active: route attribute switches to RT2")
        _set_evpn_rt(rt, EVPN_RT2, True)
        wait_checks(
            rt,
            _evpn_present_checks(
                "add-rt2-active",
                contains_rt=EVPN_RT2_FMT,
                absent_rts=[EVPN_RT1_FMT, EVPN_RT3_FMT],
            ),
            timeout=30,
        )

        step("Disable advertise evpn route: route is withdrawn even though EVPN RT exists")
        _set_advertise(rt, False)
        wait_checks(rt, [_evpn_absent_check("EVPN route withdrawn after no advertise evpn route")], timeout=30)
        hold_check(
            rt,
            device="r1",
            command="show bgp route af evpn",
            duration=6,
            interval=2,
            not_contains=[EVPN_KEY],
            label="EVPN route remains absent while advertise is disabled",
        )

        step("Change EVPN RT while advertise is disabled: delete RT2, add RT3, no route should leak")
        _set_evpn_rt(rt, EVPN_RT2, False)
        _set_evpn_rt(rt, EVPN_RT3, True)
        hold_check(
            rt,
            device="r1",
            command="show bgp route af evpn",
            duration=6,
            interval=2,
            not_contains=[EVPN_KEY],
            label="EVPN RT changes while advertise disabled do not export route",
        )

        step("Enable advertise after RT is already configured: route appears with the latest RT3")
        _set_advertise(rt, True)
        wait_checks(
            rt,
            _evpn_present_checks(
                "advertise-after-rt3",
                contains_rt=EVPN_RT3_FMT,
                absent_rts=[EVPN_RT1_FMT, EVPN_RT2_FMT],
            ),
            timeout=30,
        )

        step("Delete RT3 after advertise is enabled: route remains, RT3 disappears from attributes")
        _set_evpn_rt(rt, EVPN_RT3, False)
        wait_checks(
            rt,
            _evpn_present_checks(
                "delete-rt3-active",
                absent_rts=[EVPN_RT1_FMT, EVPN_RT2_FMT, EVPN_RT3_FMT],
            ),
            timeout=30,
        )

        step("Final withdraw via no advertise evpn route")
        _set_advertise(rt, False)
        wait_checks(rt, [_evpn_absent_check("final EVPN route withdraw")], timeout=30)

        print("EVPN VRF export RT/advertise timing and short-key query check passed.")
    finally:
        if not should_skip_cleanup():
            _cleanup(rt)
