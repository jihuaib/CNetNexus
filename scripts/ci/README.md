# CI Module Local Run

This directory contains topology-driven CI module scripts.

## Directory Layout

Each case uses its own directory:

```text
scripts/ci/modules/<module>/<top_case>/
  *.py            # check scripts (each provides run(rt, top))
  top.yaml
```

`<top_case>` should include node scale and link signature, so wiring is obvious from the
directory name.

Suggested style:

```text
<node-scale>-<link-signature>
```

Examples:

```text
two-node-r1ge1-r2ge1
two-node-r1ge1-r2ge1__r1ge2-r2ge2
three-node-r1ge1-r2ge1__r2ge2-r3ge1
```

Example:

```text
scripts/ci/modules/bgp/two-node-r1ge1-r2ge1/bgp_basic.py
scripts/ci/modules/bgp/two-node-r1ge1-r2ge1/top.yaml
```

No global `scripts/ci/topologies` is used.

Each `*.py` script in a case directory must expose:

```python
def run(rt, top) -> None:
    ...
```

Runner behavior per case directory:

- load `top.yaml` once
- start runtime/containers once
- run all `*.py` checks in that case directory
- cleanup runtime once after all checks

## Prerequisites

- Docker
- Python 3.11+
- `pyyaml`

```bash
python3 -m pip install --upgrade pip
python3 -m pip install pyyaml
```

Build local image:

```bash
docker build --target production -t netnexus-ci:localtest .
```

## Run Single Module

Example:

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --modules-dir scripts/ci/modules/bgp/two-node-r1ge1-r2ge1 \
  --report-dir scripts/ci/reports/single-case
```

Common options:

- `--keep`: keep case containers/networks for debugging
- `--cmd-timeout <sec>`: runtime CLI command timeout
- `--connect-timeout <sec>`: runtime CLI initial connect timeout
- `--verbose-modules`: enable low-level CLI debug logs (for example `... rx N bytes ...`)

## Bring Up Topology Only (No Module Scripts)

Use this when you only want local docker topology + interface IP config from `top.yaml`:

```bash
scripts/dev/top-up.sh \
  --top scripts/ci/modules/if/two-node-r1ge1-r2ge1/top.yaml \
  --image netnexus-ci:localtest
```

Notes:

- default keeps containers/networks after setup
- add `--cleanup` to remove resources before exit
- add `--pull` to force `docker pull` first
- uses image built-in `if_map.conf.gns3` (no runtime override in `top-up`)
- script will print per-device CLI connect targets automatically (`telnet <mgmt-ip> 3788`)
- add `--publish-cli` (or `--publish-cli <base-port>`) to expose CLI on host, for example `127.0.0.1:13788`

## Run All Modules (Scan + Execute)

Recommended local entry script:

```bash
scripts/ci/run_all.sh
```

Common usage:

```bash
# Skip build and use an existing image
scripts/ci/run_all.sh --no-build --image netnexus-ci:latest

# Keep resources for debugging
scripts/ci/run_all.sh --keep
```

Equivalent raw command:

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --report-dir scripts/ci/reports \
  --cmd-timeout 30 \
  --connect-timeout 60
```

Enable verbose runtime logs only when needed:

```bash
scripts/ci/run_all.sh --verbose-modules
```

By default, command send/response logs are kept for troubleshooting (including config/show command output).

`module_runner.py` will:

- scan case directories that contain `top.yaml`
- for each case: start runtime once, run all `*.py` checks, then cleanup once
- continue running remaining cases even if one check fails
- return non-zero exit code if any check fails

## Report Output

After run, report files are generated under `--report-dir`:

- `report.html`: human-readable execution report
- `summary.json`: structured result summary
- `logs/*.log`: full stdout/stderr per module
- `containers/<case>/<container>/docker.log`: container stdout/stderr
- `containers/<case>/<container>/modules/*.log`: per-module logs copied from `/opt/netnexus/log/*.log`

`report.html` groups output by step markers like `===== STEP: ... =====` and renders each step as a collapsible block. Use `step("...")` from `scripts/ci/module_api.py` in module scripts.

## Pass Extra Args To All Modules

Common flags:

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --report-dir scripts/ci/reports \
  --keep \
  --cmd-timeout 30 \
  --connect-timeout 90
```

## Cleanup Behavior

- Default: each module clears its containers and networks in `finally`.
- If `--keep` is passed to a module, resources are preserved for troubleshooting.
- One-shot cleanup for stale CI resources:

```bash
scripts/dev/cleanup-topology.sh
```

- Preview only (no deletion):

```bash
scripts/dev/cleanup-topology.sh --dry-run
```
