# ISIS CLI Documentation

This document describes the ISIS commands provided by the ISIS module (module-id: 9).

## 1. Instance Commands (`config` / `config-isis-*`)

### 1.1 `isis <tag>`
Creates (or enters) an ISIS instance and switches to `config-isis-<tag>` view.

- **Usage**: `isis <tag>`
- **View**: `config`
- **Parameters**:
  - `<tag>`: Instance tag (`uint`, 1-4294967295)

### 1.2 `no isis [<tag>]`
Deletes an ISIS instance.

- **Usage**: `no isis` / `no isis <tag>`
- **View**: `config`, `config-isis-*`
- **Notes**:
  - In `config-isis-*` view, omitting `<tag>` deletes the current context instance.

## 2. ISIS View Commands (`config-isis-*`)

### 2.1 `net <net>`
Sets NET/System-ID string for the current instance.

- **Usage**: `net <net>`
- **View**: `config-isis-*`

### 2.2 `is-type { level-1 | level-2 | level-1-2 }`
Sets the instance level type.

- **Usage**: `is-type level-1` / `is-type level-2` / `is-type level-1-2`
- **View**: `config-isis-*`

### 2.3 `af { ipv4 | ipv6 }`
Enables IPv4 or IPv6 AF on the instance.

- **Usage**: `af ipv4` / `af ipv6`
- **View**: `config-isis-*`

### 2.4 `no af { ipv4 | ipv6 }`
Disables IPv4 or IPv6 AF on the instance.

- **Usage**: `no af ipv4` / `no af ipv6`
- **View**: `config-isis-*`

## 3. Interface View Commands (`config-if-*` / `config-if-loop*`)

ISIS interface commands are split by IP type and instance tag.

- **IPv4 family command prefix**: `isis ... <tag> [...]`
- **IPv6 family command prefix**: `isis ipv6 ... <tag> [...]`
- **View**: `config-if-*`, `config-if-loop*`

### 3.1 Enable/Disable
- **IPv4**: `isis enable <tag>` / `no isis enable <tag>`
- **IPv6**: `isis ipv6 enable <tag>` / `no isis ipv6 enable <tag>`

### 3.2 Metric
- **IPv4**: `isis metric <tag> <metric>` / `no isis metric <tag>`
- **IPv6**: `isis ipv6 metric <tag> <metric>` / `no isis ipv6 metric <tag>`
- **`<metric>`**: `uint`, 1-16777215

### 3.3 Hello Interval
- **IPv4**: `isis hello-interval <tag> <hello-interval>` / `no isis hello-interval <tag>`
- **IPv6**: `isis ipv6 hello-interval <tag> <hello-interval>` / `no isis ipv6 hello-interval <tag>`
- **`<hello-interval>`**: `uint`, 1-65535

### 3.4 Hold Multiplier
- **IPv4**: `isis hold-multiplier <tag> <hold-multiplier>` / `no isis hold-multiplier <tag>`
- **IPv6**: `isis ipv6 hold-multiplier <tag> <hold-multiplier>` / `no isis ipv6 hold-multiplier <tag>`
- **`<hold-multiplier>`**: `uint`, 1-100

### 3.5 Passive
- **IPv4**: `isis passive <tag>` / `no isis passive <tag>`
- **IPv6**: `isis ipv6 passive <tag>` / `no isis ipv6 passive <tag>`

## 4. Show Commands (`global`)

### 4.1 `show isis [ipv4|ipv6] summary [<tag>]`
Displays ISIS instance summary.

### 4.2 `show isis [ipv4|ipv6] interface [<tag>]`
Displays ISIS interface status per instance.

### 4.3 `show isis [ipv4|ipv6] neighbor [<tag>]`
Displays learned ISIS LAN neighbors.

- Fields:
  - `Tag`: ISIS instance tag
  - `Interface`: Logical interface name
  - `Level`: `L1` or `L2`
  - `System-ID`: Neighbor system-id
  - `State`: `Init` / `Up`
  - `Hold`: Hold time (seconds)
  - `LastSeen`: Seconds since last IIH received
  - `IPv4`: Neighbor IPv4 interface address (if advertised)
  - `IPv6`: Neighbor IPv6 interface address (if advertised)

### 4.4 `show isis [ipv4|ipv6] lsdb [<tag>]`
Displays ISIS LSDB entries learned from received LSPs.

- Fields:
  - `Tag`: ISIS instance tag
  - `Rx-If`: Interface that received the latest LSP
  - `Level`: `L1` or `L2`
  - `System-ID`: LSP origin system-id
  - `Seq`: Latest LSP sequence
  - `Life`: Remaining lifetime in LSP header
  - `Cksm`: LSP checksum
  - `IPv4`: Count of IPv4 reachability entries
  - `IPv6`: Count of IPv6 reachability entries
  - `LastRx`: Seconds since latest LSP received

## 5. Route Learning Behavior (Current Stage)

- If an interface is ISIS-enabled and the instance AF is enabled:
  - Interface UP/DOWN and address ADD/DEL events are consumed from IF module.
  - IPv4/IPv6 prefixes are injected/withdrawn into Route module with protocol `ISIS`.
- If a neighbor advertises interface addresses in LAN IIH:
  - Neighbor IPv4/IPv6 addresses are learned and injected as one-hop host routes (`/32`, `/128`) with protocol `ISIS`.
  - Learned routes age out with neighbor hold timer or AF/interface state changes.
- ISIS also sends/receives LSP (L1/L2) periodically:
  - LSP carries Extended IS reachability (TLV 22) and IPv4/IPv6 reachability TLVs.
  - Prefix reachability is encoded with IPv4/IPv6 reachability TLVs.
  - Received LSPs are stored in LSDB (with raw TLVs), and SPF computes shortest paths from local system-id to LSP origins.
  - Route nexthop/outgoing interface is derived from SPF first-hop adjacency; learned IPv4/IPv6 routes are installed with accumulated path metric.
  - LSP-learned routes are withdrawn on neighbor loss, LSDB lifetime expiration, AF disable, or interface participation loss.
- Current scope:
  - Route learning is origin-keyed and based on topology SPF shortest-path computation.
  - LSP flooding, LSDB aging, and per-level SPF recomputation are implemented.
  - IPv4/IPv6 route learning from LSP prefixes is supported.
- Route table visibility:
  - `show route ipv4 proto isis`
  - `show route ipv6 proto isis`

## 6. Neighbor Discovery Behavior (Current Stage)

- ISIS worker opens raw socket + 1s timer tick.
- For ISIS-enabled interfaces (for example `isis enable <tag>` / `isis ipv6 enable <tag>`), LAN IIH is sent periodically (default hello 10s, hold-multiplier 3).
- ISIS LSP (L1/L2) is sent periodically on active non-passive ISIS interfaces.
- Received LAN IIH updates in-memory neighbor table per instance/interface/level.
- Received LSP updates are written into in-memory LSDB (`show isis [ipv4|ipv6] lsdb [<tag>]`) and used for SPF route learning.
- Newer LSPs are flooded to other active non-passive ISIS interfaces (except ingress interface).
- SPF builds topology from LSDB adjacency TLVs and computes first-hop nexthop for route installation.
- Neighbor entries age out by hold timer or are removed on interface disable/down.
- AF-specific passive (`isis passive <tag>` / `isis ipv6 passive <tag>`) controls participation per family.
- If at least one AF on an interface remains enabled and non-passive, IIH/LSP transmission still stays active on that interface.
