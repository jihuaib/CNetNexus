#!/usr/bin/env python3
"""
端到端验证 `dev swap-image <image>` 命令:

成功路径(Phase A~D):
  A. 记录初始 show version
  B. swap 到一个有效目标 tag,等容器内软件 re-exec 重连
  C. 校验 show version 的 Image 字段 = 新 tag
  D. 校验容器内 .image_tag、bin.old/lib.old/resources.old(回滚副本)已生成

失败回退路径(Phase E~G):
  E. 用一个不存在的 image tag 触发 swap → 脚本应在 image inspect 阶段就快速失败
  F. 校验 dev 没有 re-exec(仍是同一个 PID 7、Image 字段保持上一次成功 swap 的 tag)
  G. 校验所有模块仍然 READY/up,容器仍可正常响应命令

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
BOGUS_IMAGE = "netnexus-ci:does-not-exist-9999"
IMAGE_RE = re.compile(r"^\s*Image\s*:\s*(\S+)", re.MULTILINE)
PID_RE = re.compile(r"^\s*PID\s*:\s*(\d+)", re.MULTILINE)


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


def _parse_pid_field(show_version_out: str) -> str:
    m = PID_RE.search(show_version_out)
    return m.group(1) if m else ""


def _trigger_swap_and_reconnect(rt: TopologyRuntime, device: str, target_image: str,
                                reconnect_timeout: int = 180) -> None:
    """发送 dev swap-image,轮询直到 Image 字段变成 target_image(说明 execv 已生效)。

    新模型:swap 是 fire-and-forget,命令几乎立刻返回 ACK。后台线程跑脚本,
    完成后才 execv 替换进程映像 → telnet 断开。所以这里:
      1. 发命令,收 ACK(立刻返回)
      2. 在循环里反复试 `show version`,期间 telnet 可能断开(execv 时刻),
         自动重连
      3. 直到 `show version` 的 Image 字段 == target_image,说明新进程上线
    """
    cli = rt.cli_map.get(device)
    if cli is None:
        raise RuntimeError(f"device '{device}' has no active CLI session")

    host = cli.host
    port = cli.port

    # 1. 发 swap 命令(后台异步)。命令本身应该秒级返回 ACK。
    try:
        cli.cmd(f"dev swap-image {target_image}", strict=False, timeout=15)
    except Exception:
        # 极个别情况下 ACK 已发但 telnet 已被 execv 提前关掉,忽略
        pass

    # 2. 轮询 Image 字段,直到变成 target_image(或超时)
    deadline = time.time() + reconnect_timeout
    last_err: Exception | None = None
    last_image = ""
    while time.time() < deadline:
        try:
            # 当前会话没了就重连
            cur = rt.cli_map.get(device)
            if cur is None:
                cur = NetNexusCli(
                    host, port, device, cmd_timeout=rt.cmd_timeout, verbose=rt.verbose,
                    container=rt._container_name(device),
                )
                cur.connect(timeout=5)
                rt.cli_map[device] = cur

            out = cur.cmd("show version", strict=False, timeout=8)
            image = _parse_image_field(out)
            last_image = image
            if image == target_image:
                # Image 翻到目标 tag → execv 已生效;再确认所有子模块 READY
                remaining = max(10, int(deadline - time.time()))
                rt.wait_modules_ready(device, timeout=remaining)
                return
        except Exception as exc:
            last_err = exc
            old = rt.cli_map.pop(device, None)
            if old is not None:
                try:
                    old.close()
                except Exception:
                    pass
        time.sleep(2)

    mark_step_failed()
    raise RuntimeError(
        f"{device}: swap did not take effect within {reconnect_timeout}s "
        f"(last Image='{last_image}', expected='{target_image}', last_err={last_err})"
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

        step("Phase D: verify in-container .image_tag and *.old rollback copies via docker exec")
        rc, tag_text = _docker_exec_in(container, "cat /opt/netnexus/.image_tag")
        if rc != 0 or tag_text.strip() != target_image:
            raise RuntimeError(
                f".image_tag mismatch: rc={rc}, content='{tag_text.strip()}', "
                f"expected '{target_image}'"
            )

        # swap-image.sh 保留旧版为 bin.old / lib.old / resources.old(自然回滚副本)
        for sub in ("bin.old", "lib.old", "resources.old"):
            rc, _ = _docker_exec_in(container, f"test -d /opt/netnexus/{sub}")
            if rc != 0:
                raise RuntimeError(f"{sub} not generated after swap (rollback copy missing)")

        rc, _ = _docker_exec_in(container, "test -x /opt/netnexus/bin/netnexus")
        if rc != 0:
            raise RuntimeError("/opt/netnexus/bin/netnexus missing or not executable after swap")

        # ----- 失败回退验证(Phase E ~ G) -----
        # 思路:挑一个肯定不存在的 image tag 触发 swap;swap-image.sh 会在 docker
        # image inspect 阶段就直接 fail(SWAP_PHASE 始终保持 0,trap rollback 不需
        # 要做任何文件还原)。客户端预期收到错误响应、dev 不会 re-exec、所有模块
        # 状态保持。

        step("Phase E: capture pre-failure state (Image/PID) for rollback comparison")
        out_pre = cmd(rt, "r1", "show version", strict=False)
        pre_image = _parse_image_field(out_pre)
        pre_pid = _parse_pid_field(out_pre)
        if pre_image != target_image:
            raise RuntimeError(
                f"pre-failure Image expected '{target_image}', got '{pre_image}'"
            )
        if not pre_pid:
            raise RuntimeError(f"pre-failure PID not parseable from:\n{out_pre}")
        print(f"[swap-image-test] pre-failure Image='{pre_image}' PID={pre_pid}")

        step(f"Phase F: trigger 'dev swap-image {BOGUS_IMAGE}' (async; backend will fail)")
        # 异步模型下,worker 立刻发 "started" ACK,bogus 镜像的失败发生在后台线程。
        # 所以这里只断言 ACK 发回来了,真实失败验证留给 Phase G(状态未变)。
        try:
            ack_out = cmd(rt, "r1", f"dev swap-image {BOGUS_IMAGE}", strict=False, timeout=15)
        except Exception as exc:
            raise RuntimeError(f"swap-image with bogus tag should respond, but raised: {exc}")
        if "swap-image started" not in ack_out:
            raise RuntimeError(
                f"swap-image with bogus tag did not return expected 'started' ACK:\n{ack_out}"
            )
        # 给后台线程足够时间跑到 docker image inspect 失败 + trap rollback 收尾
        time.sleep(5)
        print(f"[swap-image-test] bogus swap accepted (background will fail), waited 5s for unwind")

        step("Phase G: verify dev did NOT re-exec (PID/Image unchanged) and modules still READY")
        out_post = cmd(rt, "r1", "show version", strict=False)
        post_image = _parse_image_field(out_post)
        post_pid = _parse_pid_field(out_post)
        if post_image != pre_image:
            raise RuntimeError(
                f"Image changed after failed swap: pre='{pre_image}' post='{post_image}' "
                f"(failed swap must NOT touch .image_tag)"
            )
        if post_pid != pre_pid:
            raise RuntimeError(
                f"PID changed after failed swap: pre={pre_pid} post={post_pid} "
                f"(failed swap must NOT execv)"
            )

        # 所有模块状态健康：resident=READY/up，on-demand=ON-DEMAND/down 视为正常待命。
        # 失败回退不应让任何模块进入异常状态（LOADED/REGISTERED/未知）。
        modules_out = cmd(rt, "r1", "show dev modules", strict=False)
        modules_row_re = re.compile(
            r"^\s*\d+\s+(?P<name>\S+)\s+(?P<phase>\S+)\s+\d+\s+(?P<ipc>\S+)\s+\S+\s*$"
        )
        rows = [m.groupdict() for line in modules_out.splitlines() if (m := modules_row_re.match(line))]
        bad: list[str] = []
        for r in rows:
            phase = r["phase"].upper()
            ipc = r["ipc"].lower()
            if phase == "READY" and ipc == "up":
                continue
            if phase == "ON-DEMAND" and ipc == "down":
                continue
            bad.append(f"{r['name']}(phase={r['phase']},ipc={r['ipc']})")
        if not rows:
            raise RuntimeError(f"after failed swap, could not parse module table:\n{modules_out}")
        if bad:
            raise RuntimeError(
                f"after failed swap, modules in abnormal state: {', '.join(bad)}\n{modules_out}"
            )

        # 容器内 .image_tag 仍然是上次成功 swap 的 tag,bogus tag 不该写进去
        rc, tag_text = _docker_exec_in(container, "cat /opt/netnexus/.image_tag")
        if rc != 0 or tag_text.strip() != target_image:
            raise RuntimeError(
                f"after failed swap, .image_tag should still be '{target_image}', "
                f"got rc={rc} content='{tag_text.strip()}'"
            )

        # bogus 触发的临时容器/staging 目录应该被 trap cleanup 清掉了
        rc, leftover = _docker_exec_in(
            container,
            "ls -d /tmp/nn-swap-* 2>/dev/null | head -5"
        )
        if leftover.strip():
            raise RuntimeError(
                f"failed swap left staging dirs not cleaned:\n{leftover}"
            )

        print(f"[swap-image-test] failure rollback verified: PID/Image/.image_tag/modules unchanged.")
        print("dev swap-image swap+reboot+verify+rollback cycle passed.")
    finally:
        # 清理:删除新 tag(实际镜像内容因为是同一 image id,不会被销毁)
        _docker_rmi(target_image)
