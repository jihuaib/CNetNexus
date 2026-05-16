#!/usr/bin/env python3
"""
Bring up a topology runtime from top.yaml/json only.

What it does:
- create docker networks/containers from top
- start NetNexus in each container
- auto-configure interface IPs from top links

What it does NOT do:
- does not run module check scripts
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
CI_DIR = PROJECT_ROOT / "scripts" / "ci"
if str(CI_DIR) not in sys.path:
    sys.path.insert(0, str(CI_DIR))

from top_runner import (
    DEVICE_KIND_FRR,
    DEVICE_KIND_NETNEXUS,
    FRR_IMAGE_ENV,
    TopologyRuntime,
    dump_logs,
    load_topology,
    run_cmd,
    validate_top,
)


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="scripts/dev/top-up.sh",
        description="Bring up topology only (no module scripts)",
    )
    parser.add_argument("--top", required=True, help="topology file path (.yaml/.yml/.json)")
    parser.add_argument("--image", required=False, help="docker image tag (fallback: top.image)")
    parser.add_argument(
        "--frr-image",
        required=False,
        help=f"FRR docker image tag for kind=frr nodes (fallback: ${FRR_IMAGE_ENV}, top.images.frr, or default)",
    )
    parser.add_argument("--prefix", default=f"nn-topup-{os.getpid()}", help="resource name prefix")
    parser.add_argument("--cmd-timeout", type=int, default=20, help="CLI command timeout seconds")
    parser.add_argument("--connect-timeout", type=int, default=60, help="CLI initial connect timeout seconds")
    parser.add_argument("--verbose", action="store_true", help="print command-level debug output")
    parser.add_argument("--pull", action="store_true", help="docker pull image before startup")
    parser.add_argument(
        "--publish-cli",
        nargs="?",
        type=int,
        const=13788,
        metavar="BASE_PORT",
        help="publish each device CLI to host: 127.0.0.1:(BASE_PORT + idx). Default base=13788 when omitted.",
    )
    parser.add_argument(
        "--cleanup",
        action="store_true",
        help="cleanup containers/networks before exiting (default keeps resources)",
    )
    args = parser.parse_args()

    top = load_topology(Path(args.top))
    validate_top(top)

    image = args.image or str(top.get("image", "")).strip()
    if not image:
        raise SystemExit("image is required (use --image or top.image)")

    if args.frr_image:
        os.environ[FRR_IMAGE_ENV] = args.frr_image

    rt = TopologyRuntime(
        top=top,
        image=image,
        prefix=args.prefix,
        keep=not args.cleanup,
        cmd_timeout=args.cmd_timeout,
        connect_timeout=args.connect_timeout,
        verbose=args.verbose,
        publish_cli_base=args.publish_cli,
    )

    if args.pull:
        for pull_image in sorted(set(rt.device_images.values())):
            print(f"\n===== STEP: docker pull {pull_image} =====", flush=True)
            run_cmd(["docker", "pull", pull_image])

    failed = False
    try:
        print("\n===== STEP: Topology startup (with interface IP config) =====", flush=True)
        rt.start(configure_interfaces=True)

        print("\nTopology is ready.", flush=True)

        print("\nCLI connect targets:", flush=True)
        for dev in sorted(rt.devices.keys()):
            mgmt_ip = rt.get_mgmt_ip(dev)
            kind = rt.get_device_kind(dev)
            if kind == DEVICE_KIND_NETNEXUS:
                print(f"  - {dev} (mgmt): telnet {mgmt_ip} 3788")
                host_port = rt.get_published_cli_port(dev)
                if host_port is not None:
                    print(f"  - {dev} (host): telnet 127.0.0.1 {host_port}")
            elif kind == DEVICE_KIND_FRR:
                print(f"  - {dev} (vtysh): docker exec -it {rt.container_name(dev)} vtysh")

        print("\nConfigured interfaces:", flush=True)
        for dev in sorted(rt.devices.keys()):
            eps = rt.endpoints.get(dev, [])
            if not eps:
                print(f"  - {dev}: (no interfaces from top)")
                continue
            for ep in eps:
                if ep.ip6:
                    print(f"  - {dev} {ep.if_name}: {ep.ip}/{ep.prefix}, {ep.ip6}/{ep.prefix6}")
                else:
                    print(f"  - {dev} {ep.if_name}: {ep.ip}/{ep.prefix}")

        print("\nContainers:", ", ".join(rt.container_names))
        print("Networks:", ", ".join([rt.mgmt_net] + rt.link_networks))

        if args.cleanup:
            print("Cleanup enabled (--cleanup), resources will be removed before exit.")
        else:
            print("Resources are kept. Use scripts/dev/cleanup-topology.sh to remove them.")
        return 0
    except Exception as exc:
        failed = True
        print(f"ERROR: {exc}", file=sys.stderr)
        dump_logs(rt.container_names)
        return 1
    finally:
        rt.close(failed=failed)


if __name__ == "__main__":
    sys.exit(main())
