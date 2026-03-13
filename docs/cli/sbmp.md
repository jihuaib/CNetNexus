# SBMP (BMP Server) CLI Documentation

This document describes the BMP server commands provided by the SBMP module (module-id: 8).

## 1. Configuration Commands (config view)

### 1.1 `bmp-server`
Enters the BMP server configuration view.

- **Usage**: `bmp-server`
- **View**: `config`
- **Transition**: Switches to the `config-bmp-server` view.

### 1.2 `no bmp-server`
Removes BMP server configuration, stops listening, and clears the server port setting.

- **Usage**: `no bmp-server`
- **View**: `config`

## 2. BMP Server View Commands (`config-bmp-server`)

### 2.1 `server port <port-number>`
Configures the BMP server listening port and starts the listener. If a previous port was configured, the old listener is stopped first.

- **Usage**: `server port <port-number>`
- **View**: `config-bmp-server`
- **Parameters**:
    - `<port-number>`: Listening port number (`uint`, 1-65535).

### 2.2 `no server port`
Stops the BMP server listener and removes the port configuration.

- **Usage**: `no server port`
- **View**: `config-bmp-server`

## 3. Show Commands (global view)

### 3.1 `show bmp server`
Displays the current BMP server status, including configured port and running state.

- **Usage**: `show bmp server`
- **View**: `global` (available in all views)

## 4. View Contexts

| View Name | Prompt Template | Description |
|---|---|---|
| `config-bmp-server` | `<NetNexus(config-bmp-server)>` | BMP server configuration view |
