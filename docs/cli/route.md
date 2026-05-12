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

### 2.1 `show route ipv4 [<destination>]`
Displays the IPv4 routing table with destination, mask, next hop, and metric.

- **Usage**: `show route ipv4` / `show route ipv4 <destination>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<destination>`: Filter by destination address (`string`, optional).

### 2.2 `show route ipv6 [<destination>]`
Displays the IPv6 routing table with destination, prefix length, next hop, and metric.

- **Usage**: `show route ipv6` / `show route ipv6 <destination>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<destination>`: Filter by destination address (`string`, optional).

### 2.3 `show route all`
Displays both IPv4 and IPv6 routing tables.

- **Usage**: `show route all`
- **View**: `global` (available in all views)

### 2.4 `show route ipv4 proto isis`
Filters IPv4 routes by ISIS protocol.

- **Usage**: `show route ipv4 proto isis`
- **View**: `global` (available in all views)

### 2.5 `show route ipv6 proto isis`
Filters IPv6 routes by ISIS protocol.

- **Usage**: `show route ipv6 proto isis`
- **View**: `global` (available in all views)

## 3. Protocol Filter Keywords

For `show route ipv4|ipv6 proto ...`, current protocol keywords include:

- `connected`
- `static`
- `bgp`
- `ospf`
- `isis`
