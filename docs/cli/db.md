# Database CLI Documentation

This document describes the database inspection commands provided by the DB module (module-id: 2).

## 1. Show Commands (global view)

### 1.1 `show db table-list`
Displays a list of all user tables in the netnexus.db database.

- **Usage**: `show db table-list`
- **View**: `global` (available in all views)

### 1.2 `show db table-field <table-name>`
Displays the schema (column information) of a specific table, including column ID, name, type, not-null constraint, and primary key status.

- **Usage**: `show db table-field <table-name>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<table-name>`: Table name (`dynamic(string(1-63))`). Supports Tab completion with existing table names.

### 1.3 `show db table-data <table-name>`
Displays all rows of data in a specific table.

- **Usage**: `show db table-data <table-name>`
- **View**: `global` (available in all views)
- **Parameters**:
    - `<table-name>`: Table name (`dynamic(string(1-63))`). Supports Tab completion with existing table names.

## 2. Configuration Snapshot Commands

### 2.1 `save configuration [name]`
Saves the current running configuration as a named snapshot.

- **Usage**: `save configuration [name]`
- **Output files**:
    - `data/configs/<name>.db`: SQLite running database snapshot.
    - `data/configs/<name>.cfg`: BDR text from `show current-configuration`.
- **Default name**: current startup name, or `startup` if no startup configuration is selected.
- **Save condition**: both `.db` and `.cfg` must be written successfully.

### 2.2 `startup configuration <name> {db|cfg}`
Selects which saved configuration is used on next cold boot.

- **Usage**:
    - `startup configuration <name> db`: restore `data/configs/<name>.db` into `running.db`.
    - `startup configuration <name> cfg`: boot with an empty `running.db`, then replay `data/configs/<name>.cfg` after DEV is ready.
- **Pointer file**: `data/startup.cfg` stores `<mode> <name>`.

### 2.3 `show startup configuration`
Displays the currently selected startup snapshot name and mode.

### 2.4 `show configuration replay-failures`
Displays failed startup `cfg` replay commands from the latest cold boot. Successful replay, `db` startup mode, and factory startup clear the failure list.
