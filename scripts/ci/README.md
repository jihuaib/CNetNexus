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
- `--verbose-modules`: print command-level logs

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
  --connect-timeout 60 \
  --verbose-modules
```

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

## Pass Extra Args To All Modules

Common flags:

```bash
python3 scripts/ci/module_runner.py \
  --image netnexus-ci:localtest \
  --report-dir scripts/ci/reports \
  --keep \
  --cmd-timeout 30 \
  --connect-timeout 60
```

## Cleanup Behavior

- Default: each module clears its containers and networks in `finally`.
- If `--keep` is passed to a module, resources are preserved for troubleshooting.
