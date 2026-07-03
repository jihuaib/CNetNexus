#!/usr/bin/env python3
"""
BGP/VRF linkage validation.

Covers:
  1) BGP VRF view rejects a missing VRF and accepts an existing VRF.
  2) BGP VRF AF view rejects a VRF AF without RD and accepts it after RD exists.
  3) VRF-side no vrf removes the linked BGP VRF config.
  4) VRF-side no af and no route-distinguisher remove the linked BGP VRF AF config.
     IPv4 and IPv6 unicast are both covered.
"""

from __future__ import annotations

import time

from module_api import check_output, cmd, reboot_device, require_devices, run_cmds, step  # noqa: E402
from top_runner import TopologyRuntime  # noqa: E402


BGP_AS = 65001
VRF_NAME = "ci-bgp-vrf"
RD_V4 = "65001:401"
RD_V6 = "65001:601"


def _cleanup(rt: TopologyRuntime) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            "no bgp",
            f"no vrf {VRF_NAME}",
            "end",
        ],
    )


def _expect_command_error(output: str, expected: str, label: str) -> None:
    violations = check_output(output, contains=[expected])
    if violations:
        raise AssertionError(f"{label}: expected error '{expected}', got:\n{output}")


def _extract_bgp_block(output: str) -> str:
    lines = output.replace("\r", "").splitlines()
    block: list[str] = []
    in_bgp = False
    for line in lines:
        stripped = line.strip()
        if stripped == f"bgp {BGP_AS}":
            in_bgp = True
            block = [line]
            continue
        if in_bgp and line == "!":
            break
        if in_bgp:
            block.append(line)
    return "\n".join(block)


def _vrf_af(af: str) -> str:
    if af == "ipv4-unicast":
        return "ipv4"
    if af == "ipv6-unicast":
        return "ipv6"
    return af


def _wait_bgp_config(
    rt: TopologyRuntime,
    *,
    label: str,
    contains: list[str] | None = None,
    not_contains: list[str] | None = None,
    timeout: int = 10,
) -> None:
    contains = contains or []
    not_contains = not_contains or []
    deadline = time.time() + timeout
    last_output = ""
    last_block = ""
    last_violations: list[str] = []

    while time.time() < deadline:
        last_output = cmd(rt, "r1", "show current-configuration", strict=False, timeout=20)
        last_block = _extract_bgp_block(last_output)
        last_violations = check_output(last_block, contains=contains, not_contains=not_contains)
        if not last_violations:
            return
        time.sleep(1)

    raise AssertionError(
        f"{label}: BGP config violations: {'; '.join(last_violations)}\n"
        f"BGP block:\n{last_block}\nfull output:\n{last_output}"
    )


def _create_vrf_with_af_rd(rt: TopologyRuntime, af: str, rd: str) -> None:
    vrf_af = _vrf_af(af)
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "end",
            "config",
            f"vrf {VRF_NAME}",
            f"af {vrf_af}",
            "no route-distinguisher",
            f"route-distinguisher {rd}",
            "exit",
            "exit",
            "end",
        ],
    )


def _configure_bgp_vrf_af(rt: TopologyRuntime, af: str) -> None:
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "end",
            "config",
            f"bgp {BGP_AS}",
            f"vrf {VRF_NAME}",
            f"af {af}",
            "exit",
            "exit",
            "end",
        ],
    )


def _delete_vrf_af(rt: TopologyRuntime, af: str) -> None:
    vrf_af = _vrf_af(af)
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "end",
            "config",
            f"vrf {VRF_NAME}",
            f"no af {vrf_af}",
            "exit",
            "end",
        ],
    )


def _delete_vrf_af_rd(rt: TopologyRuntime, af: str) -> None:
    vrf_af = _vrf_af(af)
    run_cmds(
        rt=rt,
        device="r1",
        commands=[
            "end",
            "config",
            f"vrf {VRF_NAME}",
            f"af {vrf_af}",
            "no route-distinguisher",
            "exit",
            "exit",
            "end",
        ],
    )


def _assert_bgp_vrf_af_removed(rt: TopologyRuntime, af: str, label: str) -> None:
    _wait_bgp_config(
        rt,
        not_contains=[f"  af {af}"],
        label=label,
    )


def _verify_af_rd_gate(rt: TopologyRuntime, af: str, rd: str) -> None:
    step(f"[{af}] BGP VRF AF must fail before VRF RD exists")
    outputs = run_cmds(
        rt=rt,
        device="r1",
        strict=False,
        commands=[
            "end",
            "config",
            f"bgp {BGP_AS}",
            f"vrf {VRF_NAME}",
            f"af {af}",
            "end",
        ],
    )
    _expect_command_error(outputs[4], "BGP Error: VRF address-family RD is not configured.", f"{af} RD gate")

    step(f"[{af}] Add VRF AF RD, then BGP VRF AF must succeed")
    _create_vrf_with_af_rd(rt, af, rd)
    _configure_bgp_vrf_af(rt, af)
    _wait_bgp_config(
        rt,
        contains=[
            f"bgp {BGP_AS}",
            f" vrf {VRF_NAME}",
            f"  af {af}",
        ],
        label=f"BGP VRF {af} config exists after RD",
    )


def _verify_no_af_linkage(rt: TopologyRuntime, af: str, rd: str) -> None:
    step(f"[{af}] VRF-side no af must remove linked BGP VRF AF")
    _create_vrf_with_af_rd(rt, af, rd)
    _configure_bgp_vrf_af(rt, af)
    _delete_vrf_af(rt, af)
    _assert_bgp_vrf_af_removed(rt, af, f"BGP VRF {af} removed after VRF no af")


def _verify_no_rd_linkage(rt: TopologyRuntime, af: str, rd: str) -> None:
    step(f"[{af}] VRF-side no route-distinguisher must remove linked BGP VRF AF")
    _create_vrf_with_af_rd(rt, af, rd)
    _configure_bgp_vrf_af(rt, af)
    _delete_vrf_af_rd(rt, af)
    _assert_bgp_vrf_af_removed(rt, af, f"BGP VRF {af} removed after VRF no RD")


def _verify_reboot_keeps_bgp_vrf_config(rt: TopologyRuntime) -> None:
    step("Recreate dual-stack BGP VRF config, reboot, then verify BGP config is restored")
    _create_vrf_with_af_rd(rt, "ipv4-unicast", RD_V4)
    _configure_bgp_vrf_af(rt, "ipv4-unicast")
    _create_vrf_with_af_rd(rt, "ipv6-unicast", RD_V6)
    _configure_bgp_vrf_af(rt, "ipv6-unicast")
    _wait_bgp_config(
        rt,
        contains=[
            f"bgp {BGP_AS}",
            f" vrf {VRF_NAME}",
            "  af ipv4-unicast",
            "  af ipv6-unicast",
        ],
        label="BGP VRF dual-stack config exists before reboot",
    )

    # reboot 后要验证 BGP VRF 配置从 DB 恢复，配置必须存活：先 save 落盘
    reboot_device(rt, "r1", timeout=120, save_config=True)
    _wait_bgp_config(
        rt,
        contains=[
            f"bgp {BGP_AS}",
            f" vrf {VRF_NAME}",
            "  af ipv4-unicast",
            "  af ipv6-unicast",
        ],
        label="BGP VRF dual-stack config restored after reboot",
        timeout=30,
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    try:
        _cleanup(rt)

        step("Configure BGP base")
        run_cmds(rt=rt, device="r1", commands=["end", "config", f"bgp {BGP_AS}", "end"])

        step("BGP VRF view must reject a missing VRF")
        outputs = run_cmds(
            rt=rt,
            device="r1",
            strict=False,
            commands=[
                "end",
                "config",
                f"bgp {BGP_AS}",
                f"vrf {VRF_NAME}",
                "end",
            ],
        )
        _expect_command_error(outputs[3], "BGP Error: VRF not found.", "missing VRF rejected")

        step("Create VRF; BGP VRF view must succeed")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "end",
                "config",
                f"vrf {VRF_NAME}",
                "exit",
                "end",
                "config",
                f"bgp {BGP_AS}",
                f"vrf {VRF_NAME}",
                "exit",
                "end",
            ],
        )
        _wait_bgp_config(
            rt,
            contains=[f"bgp {BGP_AS}", f" vrf {VRF_NAME}"],
            label="BGP VRF config exists after VRF create",
        )

        _verify_af_rd_gate(rt, "ipv4-unicast", RD_V4)
        _verify_no_af_linkage(rt, "ipv4-unicast", RD_V4)
        _verify_af_rd_gate(rt, "ipv6-unicast", RD_V6)
        _verify_no_rd_linkage(rt, "ipv6-unicast", RD_V6)
        _verify_reboot_keeps_bgp_vrf_config(rt)

        step("Recreate BGP VRF, then VRF-side no vrf must remove linked BGP VRF")
        _create_vrf_with_af_rd(rt, "ipv4-unicast", RD_V4)
        _configure_bgp_vrf_af(rt, "ipv4-unicast")
        run_cmds(
            rt=rt,
            device="r1",
            commands=[
                "end",
                "config",
                f"no vrf {VRF_NAME}",
                "end",
            ],
        )
        _wait_bgp_config(
            rt,
            not_contains=[f" vrf {VRF_NAME}", "  af ipv4-unicast"],
            label="BGP VRF removed after VRF no vrf",
        )

        print("BGP/VRF linkage check passed.")
    finally:
        _cleanup(rt)
