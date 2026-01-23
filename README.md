# NetNexus Telnet CLI Server

A C-based telnet server that provides a command-line interface (CLI) on port 3788 with a `<NetNexus>` prompt.

This project is designed with a modular architecture to support future implementation of networking protocols including BGP (Border Gateway Protocol), BMP (BGP Monitoring Protocol), and RPKI (Resource Public Key Infrastructure).

## Project Structure

```
NetNexus/
├── src/
│   ├── main.c                 # Main server entry point
│   └── cli/
│       ├── nn_cli_handler.h      # CLI interface (module-internal)
│       └── nn_cli_handler.c      # CLI implementation
├── CMakeLists.txt             # CMake build configuration
└── README.md
```

**Note**: Module-internal headers are kept within their respective `src/` subdirectories. Only cross-module public headers would be placed in a top-level `include/netnexus/` directory when needed.

## Features

- **TCP Socket Server**: Listens on port 3788 for incoming telnet connections
- **Multi-threaded**: Handles multiple concurrent client connections
- **Interactive CLI**: Provides a command-line interface with the `<NetNexus>` prompt
- **Built-in Commands**: Includes help, exit, quit, and show version commands
- **Line Editing**: Supports backspace and character echoing
- **Graceful Shutdown**: Handles SIGINT and SIGTERM signals properly

## Building

Ensure CMake is installed:

```bash
sudo apt install cmake
```

Build the project:

```bash
mkdir build
cd build
cmake ..
make
```

To clean build artifacts:

```bash
rm -rf build
```

## Running

Start the server from the build directory:

```bash
cd build
./netnexus
```

The server will start listening on port 3788. You should see:

```
NetNexus Telnet CLI Server
==========================

Server listening on port 3788
Press Ctrl+C to stop the server
```

## Connecting

From another terminal or machine, connect using telnet:

```bash
telnet localhost 3788
```

You will be greeted with:

```
========================================
  Welcome to NetNexus Telnet CLI
========================================
Type 'help' for available commands.

<NetNexus>
```

## Available Commands

- **help** or **?** - Display available commands
- **exit** or **quit** - Exit the CLI session
- **show version** - Display version information

## Architecture

The application uses a modular structure designed for extensibility:

1. **src/main.c**: Main server program that handles socket creation, binding, listening, and accepting connections. Uses pthreads for multi-client support.

2. **src/cli/nn_cli_handler.c**: CLI command processor that handles user input, command parsing, and execution.

3. **src/cli/nn_cli_handler.h**: CLI module header (internal to CLI module).

### Header Organization

- **Module-internal headers**: Kept within their respective `src/` subdirectories (e.g., `src/cli/nn_cli_handler.h`)
- **Cross-module headers**: Will be placed in `include/netnexus/` when needed for inter-module communication

### Future Modules

The project structure is prepared for future protocol implementations:

- **BGP (Border Gateway Protocol)**: `src/bgp/` with internal headers
- **BMP (BGP Monitoring Protocol)**: `src/bmp/` with internal headers  
- **RPKI (Resource Public Key Infrastructure)**: `src/rpki/` with internal headers

## Dependencies

### System Dependencies
- **C11 Compiler**: GCC recommended
- **Build System**: [CMake](https://cmake.org/) (3.10 or higher)
- **libxml2**: Used for XML configuration parsing
- **pthreads**: POSIX threads for concurrent client handling

### Installation (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install build-essential cmake libxml2-dev pkg-config
```

### Development Tools (Optional)
- **Clang Tools**: `clang-format` and `clang-tidy` for code quality
```bash
sudo apt install clang-format clang-tidy
```

## Requirements

- POSIX-compliant system (Linux, Unix, macOS)
- Network connectivity on port 3788

## Notes

- The server uses port 3788 by default
- Multiple clients can connect simultaneously
- Each client connection is handled in a separate thread
- The server can be stopped gracefully with Ctrl+C
