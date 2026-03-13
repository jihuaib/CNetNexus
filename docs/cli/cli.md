# CLI Core Commands Documentation

This document describes the core CLI commands provided by the CLI module (module-id: 3).
These are framework-level commands for navigation, display, and system shell access.

## 1. Navigation Commands

### 1.1 `config`
Enters the configuration mode from user view.

- **Usage**: `config`
- **View**: `user`
- **Transition**: Switches to the `config` view.

### 1.2 `exit`
Exits the current view and returns to the parent view. If already in the top-level user view, closes the CLI session.

- **Usage**: `exit`
- **View**: `global` (available in all views)

### 1.3 `end`
Returns directly to the user view from any nested view, clearing all context data.

- **Usage**: `end`
- **View**: `global` (available in all views)

## 2. Show Commands

### 2.1 `show cli command-info`
Displays a complete list of all registered CLI commands across all views and modules.

- **Usage**: `show cli command-info`
- **View**: `global` (available in all views)
- **Output**: Table with columns: VIEW, MODULE, COMMAND.

### 2.2 `show cli history`
Displays the global command execution history with timestamps and client IPs.

- **Usage**: `show cli history`
- **View**: `global` (available in all views)
- **Output**: Table with columns: No, Time, Command, Client IP.

### 2.3 `show current-configuration`
Displays the current running configuration collected from all business modules.

- **Usage**: `show current-configuration`
- **View**: `global` (available in all views)

### 2.4 `show cli context`
Displays the current session's context variables (TLV-encoded view state).

- **Usage**: `show cli context`
- **View**: `global` (available in all views)
- **Output**: Table with columns: ctx-id, type (INT/STR), value.

## 3. System Commands

### 3.1 `bash`
Enters an interactive bash shell session. The CLI session is paused and a PTY-bridged bash process is started. Type `exit` in bash to return to the CLI.

- **Usage**: `bash`
- **View**: `user`
