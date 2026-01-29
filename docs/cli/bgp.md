# BGP CLI Documentation

This document describes the BGP-related commands available in NetNexus.

## 1. Configuration Commands

These commands are used to configure BGP protocol settings and are available in the **CONFIG** view.

### 1.1 `bgp <as-number>`
Configures the BGP protocol with a specific AS number.

- **Usage**: `bgp <as-number>`
- **View**: `config`
- **Transition**: Switches to the `config-bgp` view.
- **Parameters**:
    - `<as-number>`: BGP Autonomous System (AS) number (1-4294967295).

### 1.2 `no bgp <as-number>`
Deletes the BGP protocol configuration for a specific AS number.

- **Usage**: `no bgp <as-number>`
- **View**: `config`
- **Parameters**:
    - `<as-number>`: BGP Autonomous System (AS) number.

## 2. Show Commands

These commands are used to display information about the BGP protocol and are available in the **USER** view.

### 2.1 `show bgp`
Displays the current BGP protocol configuration and status.

- **Usage**: `show bgp`
- **View**: `user`

### 2.2 `show bgp peer`
Displays information about BGP peers.

- **Usage**: `show bgp peer`
- **View**: `user`

## 3. View Contexts

### 3.1 BGP View (`config-bgp`)
This view is entered after executing the `bgp <as-number>` command.
- **Prompt Template**: `<NetNexus(config-bgp-%u)>`
