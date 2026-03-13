# BGP CLI Documentation

This document describes the BGP-related commands available in NetNexus.

## 1. Configuration Commands (config view)

### 1.1 `bgp <as-number>`
Configures the BGP protocol with a specific AS number and enters the BGP view.

- **Usage**: `bgp <as-number>`
- **View**: `config`
- **Transition**: Switches to the `config-bgp` view.
- **Parameters**:
    - `<as-number>`: BGP Autonomous System number (`uint`, 1-4294967295).

### 1.2 `no bgp`
Deletes the BGP protocol configuration.

- **Usage**: `no bgp`
- **View**: `config`

## 2. BGP View Commands (`config-bgp`)

These commands are available after entering the BGP view via `bgp <as-number>`.

### 2.1 `neighbor <ip-address> as <as-number>`
Configures a BGP neighbor session with a remote AS number.

- **Usage**: `neighbor <ip-address> as <as-number>`
- **View**: `config-bgp`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).
    - `<as-number>`: Remote AS number (`uint`, 1-4294967295).

### 2.2 `no neighbor <ip-address>`
Deletes a BGP neighbor session.

- **Usage**: `no neighbor <ip-address>`
- **View**: `config-bgp`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).

### 2.3 `af ipv4-unicast`
Enters the IPv4 unicast address family sub-view and creates the address family instance.

- **Usage**: `af ipv4-unicast`
- **View**: `config-bgp`
- **Transition**: Switches to the `config-bgp-af-ipv4-uni` view.

### 2.4 `af ipv6-unicast`
Enters the IPv6 unicast address family sub-view and creates the address family instance.

- **Usage**: `af ipv6-unicast`
- **View**: `config-bgp`
- **Transition**: Switches to the `config-bgp-af-ipv6-uni` view.

### 2.5 `no af ipv4-unicast` / `no af ipv6-unicast`
Deletes an address family instance and all associated neighbor entries.

- **Usage**: `no af { ipv4-unicast | ipv6-unicast }`
- **View**: `config-bgp`

### 2.6 `router-id <ip-address>`
Sets the BGP router ID for the current VRF.

- **Usage**: `router-id <ip-address>`
- **View**: `config-bgp`
- **Parameters**:
    - `<ip-address>`: Router ID as an IPv4 address (`ip`).

### 2.7 `no router-id`
Resets the BGP router ID to default.

- **Usage**: `no router-id`
- **View**: `config-bgp`

### 2.8 `timer keepalive <keepalive-time> hold <hold-time>`
Configures BGP keepalive and hold timers.

- **Usage**: `timer keepalive <keepalive-time> hold <hold-time>`
- **View**: `config-bgp`
- **Parameters**:
    - `<keepalive-time>`: Keepalive interval in seconds (`uint`, 1-65535).
    - `<hold-time>`: Hold time in seconds (`uint`, 1-65535). Must be greater than keepalive.

### 2.9 `timer connect-retry <seconds>`
Configures the connect-retry timer for BGP sessions.

- **Usage**: `timer connect-retry <seconds>`
- **View**: `config-bgp`
- **Parameters**:
    - `<seconds>`: Reconnect interval in seconds (`uint`, 1-65535).

### 2.10 `no timer keepalive` / `no timer connect-retry`
Resets the respective timer to default values.

- **Usage**: `no timer keepalive` / `no timer connect-retry`
- **View**: `config-bgp`

### 2.11 `neighbor <ip-address> open-capability as4`
Enables 4-byte AS number capability (RFC 6793) for a neighbor.

- **Usage**: `neighbor <ip-address> open-capability as4`
- **View**: `config-bgp`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).

### 2.12 `neighbor <ip-address> open-capability route-refresh`
Enables Route Refresh capability (RFC 2918) for a neighbor.

- **Usage**: `neighbor <ip-address> open-capability route-refresh`
- **View**: `config-bgp`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).

### 2.13 `no neighbor <ip-address> open-capability as4|route-refresh`
Disables the corresponding capability for a neighbor.

- **Usage**: `no neighbor <ip-address> open-capability { as4 | route-refresh }`
- **View**: `config-bgp`

## 3. Address Family View Commands (`config-bgp-af-*`)

### 3.1 `neighbor <ip-address> enable`
Enables a neighbor for the current address family.

- **Usage**: `neighbor <ip-address> enable`
- **View**: `config-bgp-af-ipv4-uni`, `config-bgp-af-ipv6-uni`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).

### 3.2 `no neighbor <ip-address>`
Disables a neighbor for the current address family.

- **Usage**: `no neighbor <ip-address>`
- **View**: `config-bgp-af-ipv4-uni`, `config-bgp-af-ipv6-uni`
- **Parameters**:
    - `<ip-address>`: Neighbor IP address (`ip`).

## 4. Show Commands (global view)

### 4.1 `show bgp [peer]`
Displays BGP protocol configuration, sessions, and address-family neighbor information.

- **Usage**: `show bgp` / `show bgp peer`
- **View**: `global`

## 5. View Contexts

| View Name | Prompt Template | Description |
|---|---|---|
| `config-bgp` | `<NetNexus(config-bgp)>` | BGP configuration view |
| `config-bgp-af-ipv4-uni` | `<NetNexus(config-bgp-af-ipv4-uni)>` | IPv4 unicast address family view |
| `config-bgp-af-ipv6-uni` | `<NetNexus(config-bgp-af-ipv6-uni)>` | IPv6 unicast address family view |
