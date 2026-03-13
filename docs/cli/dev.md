# Dev CLI Documentation

This document describes the device management commands provided by the Dev module (module-id: 1).

## 1. Show Commands

### 1.1 `show version`
Displays system version information including build profile, compiler, ASAN status, log level, and PID.

- **Usage**: `show version`
- **View**: `global` (available in all views)

### 1.2 `show dev modules`
Displays all registered modules with their ID, name, phase, port, and IPC connection status.

- **Usage**: `show dev modules`
- **View**: `global` (available in all views)
- **Output**: Table with columns: ID, Name, Phase, Port, IPC.

### 1.3 `show dev ipc <module-name>`
Displays IPC connection details for a specific module, including connection direction, state, remote address, heartbeat timestamps, and reconnect delay.

- **Usage**: `show dev ipc <module-name>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<module-name>`: Module name (`dynamic(string(1-12))`). Supports Tab completion with registered module names.

## 2. Configuration Commands

### 2.1 `sysname <hostname>`
Sets the system hostname (placeholder, not yet implemented).

- **Usage**: `sysname <hostname>`
- **View**: `config`
- **Parameters**:
    - `<hostname>`: System hostname (`string(1-63)`).

### 2.2 `dev log-level <level>`
Sets the runtime log level for all modules.

- **Usage**: `dev log-level <level>`
- **View**: `config`
- **Parameters**:
    - `<level>`: Log level (`string(4-5)`). Valid values: `debug`, `info`, `warn`, `error`.

## 3. Network Commands

### 3.1 `ping <ip-address>`
Sends 4 ICMP echo requests to the specified IPv4 address.

- **Usage**: `ping <ip-address>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<ip-address>`: Target IPv4 address (`ip`).
