# Route CLI Documentation

This document describes the static route commands provided by the Route module (module-id: 7).

## 1. Configuration Commands (config view)

### 1.1 `route ipv4 <destination> <mask> <next-hop> [<metric>]`
Adds a static IPv4 route.

- **Usage**: `route ipv4 <destination> <mask> <next-hop> [<metric>]`
- **View**: `config`
- **Parameters**:
    - `<destination>`: Destination network address (`string`).
    - `<mask>`: Subnet mask (`string`).
    - `<next-hop>`: Next hop address (`string`).
    - `<metric>`: Route metric (`uint`, 0-255, optional, default: 0).

### 1.2 `no route ipv4 <destination> <mask> [<next-hop>]`
Deletes a static IPv4 route.

- **Usage**: `no route ipv4 <destination> <mask> [<next-hop>]`
- **View**: `config`
- **Parameters**:
    - `<destination>`: Destination network address (`string`).
    - `<mask>`: Subnet mask (`string`).
    - `<next-hop>`: Next hop address (`string`, optional. If omitted, all matching routes are deleted).

### 1.3 `route ipv6 <destination> <prefix-length> <next-hop> [<metric>]`
Adds a static IPv6 route.

- **Usage**: `route ipv6 <destination> <prefix-length> <next-hop> [<metric>]`
- **View**: `config`
- **Parameters**:
    - `<destination>`: Destination network address (`string`).
    - `<prefix-length>`: Prefix length (`uint`, 0-128).
    - `<next-hop>`: Next hop address (`string`).
    - `<metric>`: Route metric (`uint`, 0-255, optional, default: 0).

### 1.4 `no route ipv6 <destination> <prefix-length> [<next-hop>]`
Deletes a static IPv6 route.

- **Usage**: `no route ipv6 <destination> <prefix-length> [<next-hop>]`
- **View**: `config`
- **Parameters**:
    - `<destination>`: Destination network address (`string`).
    - `<prefix-length>`: Prefix length (`uint`, 0-128).
    - `<next-hop>`: Next hop address (`string`, optional).

## 2. Show Commands (global view)

### 2.1 `show route {ipv4|ipv6} [vrf <vrf-name>]`
Displays the routing table for the selected address family (or both if AF is omitted).

- **Usage**: `show route ipv4`, `show route ipv6`, `show route ipv4 vrf red`
- **View**: `global` (available in all views)
- **Parameters**:

### 2.2 `show route {ipv4|ipv6} [vrf <vrf-name>] <destination> <prefix-length>`
Displays detailed route entry matching destination/prefix.

- **Usage**: `show route ipv4 vrf red 10.1.1.0 24`, `show route ipv6 vrf blue 2001:db8:: 64`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<destination>`: Filter by destination address (`string`)
    - `<prefix-length>`: Prefix length (`uint`)
    - `<vrf-name>`: Optional VRF name after `vrf` keyword

### 2.3 `show route {ipv4|ipv6} [vrf <vrf-name>] proto <proto>`
Filters routes by protocol type.

- **Usage**: `show route ipv4 proto isis`, `show route ipv6 vrf red proto bgp`, `show route ipv4 vrf blue proto static`
- **View**: `global` (available in all views)
- **Valid** `<proto>` values: `static`, `bgp`, `ospf`, `connected`, `isis`

### 2.4 `show route summary [vrf <vrf-name>] {ipv4|ipv6}`
Displays route summary statistics.

- **Usage**: `show route summary`, `show route summary ipv4`, `show route summary vrf red ipv6`
- **View**: `global` (available in all views)

### 2.5 `show route subscribe [vrf <vrf-name>] {ipv4|ipv6}`
Displays route subscription information.

- **Usage**: `show route subscribe`, `show route subscribe ipv4`, `show route subscribe vrf red ipv6`
- **View**: `global` (available in all views)

## 3. Protocol Filter Keywords

For `show route proto ...`, current protocol keywords include:

- `connected`
- `static`
- `bgp`
- `ospf`
- `isis`
