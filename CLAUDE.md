# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libxml2-dev pkg-config

# Build
mkdir build && cd build && cmake .. && make

# Build and run
cmake --build . --target run

# Clean
rm -rf build
```

## Code Quality

```bash
./scripts/format-code.sh       # Format all code with clang-format
./scripts/check-format.sh      # Verify formatting
./scripts/run-clang-tidy.sh    # Run static analysis
```

## Running

```bash
cd build/bin && ./netnexus     # Starts server on port 3788
telnet localhost 3788          # Connect to CLI
```

## Build Output

After building, the following artifacts are created:
- `build/bin/netnexus` - Main executable
- `build/lib/libnn_cfg.so` - CLI library (CLI framework)
- `build/lib/libnn_core.so` - Core library (module registry)
- `build/lib/libnn_base.so` - Base module
- `build/lib/libnn_bgp.so` - BGP module
- `build/lib/libnn_dev.so` - Dev module

## Architecture

NetNexus is a modular telnet CLI server for networking protocols. Key architectural concepts:

### Module System
Modules self-register at load time using `__attribute__((constructor))`. Each module provides:
- A registration call to `nn_cli_register_module(name, xml_path)`
- An XML config file defining commands (in module's directory)

See `src/modules/base/base_module.c` for the pattern.

### View Hierarchy
Views represent CLI modes (USER, CONFIG, BGP, etc.). Each view has:
- Its own command tree
- A prompt template with `{hostname}` placeholder
- Optional parent view for inheritance

Views and commands are defined in XML config files, not C code.

### Command Tree
Commands are hierarchical trees where root-to-leaf paths form complete commands. Nodes are either:
- `KEYWORD`: Fixed tokens (e.g., "show", "configure")
- `ARGUMENT`: Variable parameters (e.g., `<hostname>`)

### Directory Structure
```
src/
├── main.c                      # TCP server, threading, signal handling
├── cfg/                        # CLI library (libnn_cfg.so)
│   ├── nn_cli_handler.c/h      # Client sessions, command execution
│   ├── nn_cli_tree.c/h         # Command tree matching
│   ├── nn_cli_view.c/h         # View hierarchy management
│   ├── nn_cli_element.c/h      # CLI element handling
│   └── nn_cli_xml_parser.c/h   # XML config parsing
├── core/                       # Core library (libnn_core.so)
│   └── nn_module_registry.c    # Module auto-discovery
└── modules/
    ├── base/                   # Base module (libnn_base.so)
    ├── bgp/                    # BGP module (libnn_bgp.so)
    └── dev/                    # Dev module (libnn_dev.so)
```

### Library Dependencies
```
netnexus (executable)
├── libnn_cfg.so (CLI framework)
│   └── libxml2, pthread
├── libnn_core.so (module registry)
│   └── libnn_cfg.so
├── libnn_base.so → libnn_core.so
├── libnn_bgp.so  → libnn_core.so
└── libnn_dev.so  → libnn_core.so
```

### Globals
- `g_view_tree`: Root of view hierarchy
- `g_hostname`: System hostname for prompts

## Naming Conventions

- `nn_cli_*`: CLI subsystem functions
- `nn_*`: Module registry functions
- `cmd_*`: Command handler functions
- `*_t`: Type definitions
- `g_*`: Global variables
- `UPPER_CASE`: Macros and enums
- `lower_case`: Functions, variables, structs

## Code Style

- C11, LLVM-based formatting with Allman braces
- 4-space indent, 120-char line limit
- Right-aligned pointers (`char *ptr`)
- Braces required for all control statements
