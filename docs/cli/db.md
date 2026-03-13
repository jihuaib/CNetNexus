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
