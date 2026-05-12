# VRF CLI Documentation

This document describes the VRF management commands provided by the VRF module (module-id: 4).

## 1. Configuration Commands (config view)

### 1.1 `vrf <vrf-name>`
Creates a VRF instance and enters the VRF configuration view. If the VRF already exists, directly enters the view.

- **Usage**: `vrf <vrf-name>`
- **View**: `config`
- **Transition**: Switches to the `config-vrf-<name>` view.
- **Parameters**:
    - `<vrf-name>`: VRF name (`dynamic(string(1-63))`). Supports Tab completion. The public VRF cannot be manually created.

### 1.2 `no vrf <vrf-name>`
Deletes a VRF instance. The public VRF cannot be deleted.

- **Usage**: `no vrf <vrf-name>`
- **View**: `config`
- **Parameters**:
    - `<vrf-name>`: VRF name (`dynamic(string(1-63))`).

## 2. Show Commands (global view)

### 2.1 `show vrf`
Displays a table of all VRF instances with VRF-ID and name.

- **Usage**: `show vrf`
- **View**: `global` (available in all views)

### 2.2 `show vrf <vrf-name>`
Displays detailed information for a specific VRF instance.

- **Usage**: `show vrf <vrf-name>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<vrf-name>`: VRF name (`dynamic(string(1-63))`). Supports Tab completion.

## 3. View Contexts

| View Name | Prompt Template | Description |
|---|---|---|
| `config-vrf-*` | `<NetNexus(config-vrf-{ctx:5})>` | VRF configuration view. `{ctx:5}` displays the VRF name. |
