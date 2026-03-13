# SBMP (BMP Server) CLI Documentation

This document describes BMP server commands provided by the SBMP module (module-id: 8).

## 1. Configuration Commands (config view)

### 1.1 `bmp-server`
Enters BMP server configuration view.

- **Usage**: `bmp-server`
- **View**: `config`
- **Transition**: switches to `config-bmp-server`

### 1.2 `no bmp-server`
Removes BMP server configuration and stops listener.

- **Usage**: `no bmp-server`
- **View**: `config`

## 2. BMP Server View Commands (`config-bmp-server`)

### 2.1 `server port <port-number>`
Configures BMP server listening port and starts listener.

- **Usage**: `server port <port-number>`
- **View**: `config-bmp-server`
- **Parameters**:
    - `<port-number>`: listening port (`uint`, 1-65535)

### 2.2 `no server port`
Stops listener and removes configured port.

- **Usage**: `no server port`
- **View**: `config-bmp-server`

## 3. Show Commands (global view)

### 3.1 `show bmp-server`
Shows BMP server status and runtime totals.

### 3.2 `show bmp-server client [<client-id>]`
Shows BMP client summary or one client detail.

### 3.3 `show bmp-server peer [client <client-id>] [peer <peer-id>]`
Shows peer runtime state with optional client/peer filters.

### 3.4 `show bmp-server route af { ipv4-unicast | ipv6-unicast } [client <client-id>] [peer <peer-id>] [policy { pre | post | loc-rib }]`
Shows in-memory mirrored routes with optional client/peer filters.

- `policy pre`: pre-policy route view (Adj-RIB-In)
- `policy post`: post-policy route view
- `policy loc-rib`: Loc-RIB route view
- policy omitted: show pre/post/loc-rib

## 4. View Contexts

| View Name | Prompt Template | Description |
|---|---|---|
| `config-bmp-server` | `<NetNexus(config-bmp-server)>` | BMP server configuration view |
