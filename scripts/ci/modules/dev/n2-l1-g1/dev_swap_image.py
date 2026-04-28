#!/usr/bin/env python3
"""
端到端验证 `dev swap-image <image>` 命令:

1. 在宿主机给当前 CI 镜像打一个新 tag(swap 目标),不改实际内容
2. r1 上运行 `show version`,记录初始 Image 字段(通常为 n/a)
3. 向 r1 发 `dev swap-image <new-tag>`,触发 swap-image.sh 替换 bin/lib/resources
   并执行软件 reboot
4. 重连 telnet,等待所有模块 READY
5. 再次 `show version`,断言 Image 字段=新 tag
6. 通过 `docker exec` 验证容器内 `.image_tag` 内容和 `.prev/{bin,lib,resources}` 已生成
7. 清理 swap 目标 tag

依赖:
- 容器 `docker run` 时挂了 `/var/run/docker.sock`(top_runner.py 已统一加上)
- 镜像内已包含 docker CLI 和 `/opt/netnexus/scripts/swap-image.sh`(Dockerfile 已加)
"""

from __future__ import annotations

import re
import subprocess
import time

from module_api import cmd, mark_step_failed, require_devices, step  # noqa: E402
from top_runner import NetNexusCli, TopologyRuntime, run_cmd  # noqa: E402


SWAP_TAG_SUFFIX = "swaptest"
IMAGE_RE = re.compile(r"^\s*Image\s*:\s*(\S+)", re.MULTILINE)


def _build_swap_target(base_image: str) -> str:
    """根据当前镜像名生成一个 swap 目标 tag(同一镜像不同 tag)。"""
    if ":" in base_image:
        repo, _ = base_image.rsplit(":", 1)
    else:
        repo = base_image
    return f"{repo}:{SWAP_TAG_SUFFIX}"


def _docker_tag(src: str, dst: str) -> None:
    run_cmd(["docker", "tag", src, dst])


def _docker_rmi(tag: str) -> None:
    subprocess.run(
        ["docker", "rmi", "-f", tag],
        text=True, capture_output=True, check=False,
    )


def _docker_exec_in(container: str, sh_cmd: str) -> tuple[int, str]:
    proc = subprocess.run(
        ["docker", "exec", container, "/bin/sh", "-lc", sh_cmd],
        text=True, capture_output=True, check=False,
    )
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def _parse_image_field(show_version_out: str) -> str:
    m = IMAGE_RE.search(show_version_out)
    return m.group(1) if m else ""


def _trigger_swap_and_reconnect(rt: TopologyRuntime, device: str, target_image: str,
                                reconnect_timeout: int = 120) -> None:
    """发送 dev swap-image,等容器内软件 reboot 完成,重新建立 CLI 会话。"""
    cli = rt.cli_map.get(device)
    if cli is None:
        raise RuntimeError(f"device '{device}' has no active CLI session")

    host = cli.host
    port = cli.port

    triggered_at = time.time()
    try:
        cli.cmd(f"dev swap-image {target_image}", strict=False, timeout=60)
    except Exception:
        # swap 期间 CLI 会主动断开,正常现象;由下面的 reconnect 决定成败
        pass
    finally:
        cli.close()
        rt.cli_map.pop(device, None)

    deadline = time.time() + reconnect_timeout
    last_err: Exception | None = None
    while time.time() < deadline:
        # swap 涉及 dlclose+dlopen,等几秒再重连,避免半状态命中
        if time.time() < triggered_at + 3.0:
            time.sleep(0.5)
            continue
        new_cli: NetNexusCli | None = None
        try:
            new_cli = NetNexusCli(host, port, device, cmd_timeout=rt.cmd_timeout, verbose=rt.verbose)
            new_cli.connect(timeout=min(10, max(2, int(deadline - time.time()))))
            new_cli.cmd("show version", strict=False, timeout=8)
            time.sleep(1)
            new_cli.cmd("show version", strict=False, timeout=8)
            rt.cli_map[device] = new_cli
            from module_runner import wait_device_modules_ready
            remaining = max(10, int(deadline - time.time()))
            wait_device_modules_ready(rt, device, timeout=remaining)
            return
        except Exception as exc:
            last_err = exc
            try:
                if new_cli is not None:
                    new_cli.close()
                    rt.cli_map.pop(device, None)
            except Exception:
                pass
            time.sleep(1)

    mark_step_failed()
    raise RuntimeError(
        f"{device}: failed to reconnect after swap-image within {reconnect_timeout}s: {last_err}"
    )


def run(rt: TopologyRuntime, top: dict[str, object]) -> None:
    require_devices(top, ("r1",))

    base_image = rt.image
    target_image = _build_swap_target(base_image)
    container = rt.container_name("r1")

    try:
        step(f"Tag a swap target on host: {base_image} -> {target_image}")
        _docker_tag(base_image, target_image)

        step("Phase A: capture initial 'show version' output")
        out_a = cmd(rt, "r1", "show version", strict=False)
        if "NetNexus Version Information" not in out_a:
            raise RuntimeError(f"unexpected show version output:\n{out_a}")
        if "Image" not in out_a:
            raise RuntimeError(
                f"'show version' missing 'Image' line — did dev_cli.c expose it?\n{out_a}"
            )
        before_image = _parse_image_field(out_a)
        print(f"[swap-image-test] initial Image = '{before_image}'")

        step(f"Phase B: 'dev swap-image {target_image}' and wait for reboot")
        _trigger_swap_and_reconnect(rt, "r1", target_image, reconnect_timeout=180)

        step("Phase C: verify 'show version' Image field equals the new tag")
        out_b = cmd(rt, "r1", "show version", strict=False)
        after_image = _parse_image_field(out_b)
        if after_image != target_image:
            raise RuntimeError(
                f"Image field mismatch: expected '{target_image}', got '{after_image}'\n"
                f"full output:\n{out_b}"
            )
        print(f"[swap-image-test] after swap Image = '{after_image}' OK")

        step("Phase D: verify in-container .image_tag and .prev/* via docker exec")
        rc, tag_text = _docker_exec_in(container, "cat /opt/netnexus/.image_tag")
        if rc != 0 or tag_text.strip() != target_image:
            raise RuntimeError(
                f".image_tag mismatch: rc={rc}, content='{tag_text.strip()}', "
                f"expected '{target_image}'"
            )

        for sub in ("bin", "lib", "resources"):
            rc, _ = _docker_exec_in(container, f"test -d /opt/netnexus/.prev/{sub}")
            if rc != 0:
                raise RuntimeError(f".prev/{sub} not generated after swap")

        rc, _ = _docker_exec_in(container, "test -x /opt/netnexus/bin/netnexus")
        if rc != 0:
            raise RuntimeError("/opt/netnexus/bin/netnexus missing or not executable after swap")

        print("dev swap-image swap+reboot+verify cycle passed.")
    finally:
        # 清理:删除新 tag(实际镜像内容因为是同一 image id,不会被销毁)
        _docker_rmi(target_image)
