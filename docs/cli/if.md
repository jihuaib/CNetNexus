# Interface CLI Documentation

This document describes the interface management commands provided by the IF module (module-id: 4).

## 1. Interface Entry Commands (config view)

### 1.1 `if { GE-1 | GE-2 | GE-3 | GE-4 }`
Enters the interface configuration view for a specific GigabitEthernet interface.

- **Usage**: `if GE-1` / `if GE-2` / `if GE-3` / `if GE-4`
- **View**: `config`
- **Transition**: Switches to the `config-if-GE-<N>` view.

## 2. Interface Configuration Commands (`config-if-GE-*` view)

### 2.1 `ip address <ip-address> <prefix-len>`
Configures an IPv4 address on the current interface.

- **Usage**: `ip address <ip-address> <prefix-len>`
- **View**: `config-if-GE-*`
- **Parameters**:
    - `<ip-address>`: IPv4 address (`ip`).
    - `<prefix-len>`: Prefix length (`integer`, 0-32).

### 2.2 `shutdown`
Administratively disables the current interface.

- **Usage**: `shutdown`
- **View**: `config-if-GE-*`

### 2.3 `no shutdown`
Administratively enables the current interface.

- **Usage**: `no shutdown`
- **View**: `config-if-GE-*`

## 3. Show Commands (global view)

### 3.1 `show if`
Displays a summary table of all interfaces with name, state, and IP address.

- **Usage**: `show if`
- **View**: `global` (available in all views)

### 3.2 `show if { GE-1 | GE-2 | GE-3 | GE-4 }`
Displays detailed information for a specific interface, including name, type, state, IP address, MAC address, and MTU.

- **Usage**: `show if GE-1` / `show if GE-2` / `show if GE-3` / `show if GE-4`
- **View**: `global` (available in all views)

## 4. View Contexts

| View Name | Prompt Template | Description |
|---|---|---|
| `config-if-GE-*` | `<NetNexus(config-if-GE-{ctx:4})>` | Interface configuration view. `{ctx:4}` displays the interface index. |
