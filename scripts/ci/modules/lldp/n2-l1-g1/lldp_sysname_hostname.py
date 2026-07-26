#!/usr/bin/env python3
"""
LLDP system-name tracking when NetNexus sysname changes kernel hostname.

Topology: r1 --- GE-1 --- r2

Coverage:
- default sysname is advertised through LLDP
- LLDP neighbor detail learns the updated peer system name
- no sysname restores the default hostname and LLDP advertisement
"""

from __future__ import annotations

from module_api import check_output, require_devices, run_cmds, step, wait_checks  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


GE_IF = "GE-1"
DEFAULT_SYSNAME = "NetNexus"
R1_SYSNAME = "R1SYS"
R2_SYSNAME = "R2SYS"
TX_INTERVAL_SEC = 5
HOLD_MULTIPLIER = 2


def _cleanup(rt: TopologyRuntime, *, restore_topology_sysname: bool = False) -> None:
    step("Cleanup LLDP/sysname config")
    for dev in ("r1", "r2"):
        run_cmds(
            rt=rt,
            device=dev,
            strict=False,
            commands=[
                "end",
                "config",
                f"if {GE_IF}",
                "no lldp admin-status",
                "lldp enable",
                "exit",
                "no lldp hold-multiplier",
                "no lldp timer",
                "no lldp",
                "no sysname",
                "end",
            ],
        )
        if restore_topology_sysname:
            _set_sysname(rt, device=dev, sysname=dev)


def _configure_lldp(rt: TopologyRuntime) -> None:
    for dev in ("r1", "r2"):
        run_cmds(
            rt=rt,
            device=dev,
            commands=[
                "config",
                "lldp",
                f"lldp timer {TX_INTERVAL_SEC}",
                f"lldp hold-multiplier {HOLD_MULTIPLIER}",
                f"if {GE_IF}",
                "lldp enable",
                "lldp admin-status txrx",
                "exit",
                "end",
            ],
        )


def _set_sysname(rt: TopologyRuntime, *, device: str, sysname: str) -> None:
    run_cmds(rt=rt, device=device, commands=["end", "config", f"sysname {sysname}", "end"])


def _clear_sysname(rt: TopologyRuntime, *, device: str) -> None:
    run_cmds(rt=rt, device=device, commands=["end", "config", "no sysname", "end"])


def _wait_lldp_peer_name(rt: TopologyRuntime, *, observer: str, expected_peer_name: str, old_peer_name: str | None) -> None:
    check = {
        "device": observer,
        "command": "show lldp neighbors detail",
        "contains": [f"Interface: {GE_IF}", f"System name : {expected_peer_name}", "TTL"],
        "label": f"{observer} learns peer system name {expected_peer_name}",
    }
    if old_peer_name is not None and old_peer_name != expected_peer_name:
        check["not_contains"] = [f"System name : {old_peer_name}"]
    wait_checks(rt, [check], timeout=45, interval=3)


def _assert_current_config_sysname(rt: TopologyRuntime, *, device: str, expected: str | None) -> None:
    out = run_cmds(rt=rt, device=device, commands=["show current-configuration"])[0]
    contains = [f"sysname {expected}"] if expected else []
    not_contains = ["sysname "] if expected is None else []
    violations = check_output(out, contains=contains, not_contains=not_contains)
    if violations:
        raise AssertionError(f"{device} current config sysname check failed: {'; '.join(violations)}\n{out}")


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1", "r2"))

    try:
        _cleanup(rt)

        step("Configure LLDP on both routers")
        _configure_lldp(rt)

        step("Verify default sysname is learned by LLDP")
        _wait_lldp_peer_name(rt, observer="r1", expected_peer_name=DEFAULT_SYSNAME, old_peer_name=None)
        _wait_lldp_peer_name(rt, observer="r2", expected_peer_name=DEFAULT_SYSNAME, old_peer_name=None)

        step("Change sysname on both routers")
        _set_sysname(rt, device="r1", sysname=R1_SYSNAME)
        _set_sysname(rt, device="r2", sysname=R2_SYSNAME)
        _assert_current_config_sysname(rt, device="r1", expected=R1_SYSNAME)
        _assert_current_config_sysname(rt, device="r2", expected=R2_SYSNAME)

        step("Verify LLDP learns changed peer sysname")
        _wait_lldp_peer_name(rt, observer="r1", expected_peer_name=R2_SYSNAME, old_peer_name=DEFAULT_SYSNAME)
        _wait_lldp_peer_name(rt, observer="r2", expected_peer_name=R1_SYSNAME, old_peer_name=DEFAULT_SYSNAME)

        step("Restore default sysname and verify LLDP updates back")
        _clear_sysname(rt, device="r1")
        _clear_sysname(rt, device="r2")
        _assert_current_config_sysname(rt, device="r1", expected=None)
        _assert_current_config_sysname(rt, device="r2", expected=None)
        _wait_lldp_peer_name(rt, observer="r1", expected_peer_name=DEFAULT_SYSNAME, old_peer_name=R2_SYSNAME)
        _wait_lldp_peer_name(rt, observer="r2", expected_peer_name=DEFAULT_SYSNAME, old_peer_name=R1_SYSNAME)

    finally:
        _cleanup(rt, restore_topology_sysname=True)

    print("LLDP sysname hostname tracking check passed.")
