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
- `build/lib/libnn_utils.so` - Utility library
- `build/lib/libnn_bgp.so` - BGP module
- `build/lib/libnn_dev.so` - Dev module

## Architecture

NetNexus is a modular telnet CLI server for networking protocols. Key architectural concepts:

### Module System
Modules self-register at load time using `__attribute__((constructor))`. Each module provides:
- A registration call to `nn_cli_register_module(name, xml_path)`
- An XML config file defining commands (in module's directory)

See [nn_dev_module.c](src/dev/nn_dev_module.c) for the pattern.

### View Hierarchy
Views represent CLI modes (USER, CONFIG, BGP, etc.). Each view has:
- Its own command tree
- A prompt template with `{hostname}` placeholder
- Optional parent view for inheritance

Views and commands are defined in XML config files, not C code.

### Command Tree
Commands are hierarchical trees where root-to-leaf paths form complete commands. Nodes are either:
- `KEYWORD`: Fixed tokens (e.g., "show", "config")
- `ARGUMENT`: Variable parameters (e.g., `<hostname>`)

Each tree node has an `is_end_node` flag indicating whether it's a valid command completion point. This allows both "show bgp" and "show bgp peer" to be valid commands independently - the "bgp" node is marked as an end node even though it has children.

When building commands from XML `<expression>` elements, the last element in each expression is automatically marked as an end node by the parser.

### CLI Input Handling
The CLI supports full line editing with cursor positioning:
- **ANSI Escape Sequences**: Arrow keys are detected using a state machine (STATE_NORMAL → STATE_ESC → STATE_CSI)
- **Cursor Movement**: Up/Down arrows browse command history; Left/Right arrows move cursor within current line
- **Mid-line Editing**: Characters can be inserted or deleted at any cursor position using `memmove()` to shift buffer contents
- **Tab/Help**: Work based on cursor position (only text before cursor is used for matching)
- **History**: Session-specific (20 commands) and global (200 commands) history with timestamps and client IPs

Key functions in [nn_cli_handler.c](src/cfg/nn_cli_handler.c):
- `handle_arrow_up/down/left/right()` - Arrow key handlers
- `redraw_from_cursor()` - Redraw line after mid-line edits
- `nn_cli_session_history_*()` - Session history management
- `nn_cli_global_history_*()` - Global history with pthread mutex

### Directory Structure
```
src/
├── main.c                      # TCP server, threading, signal handling
├── cfg/                        # CLI library (libnn_cfg.so)
│   ├── nn_cli_handler.c/h      # Client sessions, command execution
│   ├── nn_cli_history.c/h      # Command history management
│   ├── nn_cli_tree.c/h         # Command tree matching
│   ├── nn_cli_view.c/h         # View hierarchy management
│   ├── nn_cli_element.c/h      # CLI element handling
│   ├── nn_cli_xml_parser.c/h   # XML config parsing
│   └── commands.xml            # Core CLI commands
├── utils/                      # Utility library (libnn_utils.so)
│   └── nn_path_utils.c/h       # Path utilities
├── interface/                  # Interface definitions
├── bgp/                        # BGP module (libnn_bgp.so)
│   ├── nn_bgp_module.c/h
│   └── commands.xml
└── dev/                        # Dev module (libnn_dev.so)
    ├── nn_dev_module.c/h
    └── commands.xml
```

### Library Dependencies
```
netnexus (executable)
├── libnn_cfg.so (CLI framework)
│   └── libxml2, pthread
├── libnn_utils.so (utilities)
├── libnn_bgp.so (BGP module)
└── libnn_dev.so (Dev module)
```

### Globals
- `g_nn_cfg_local->view_tree`: Root of view hierarchy

## Naming Conventions

- `nn_cli_*`: CLI subsystem functions
- `nn_*`: Module registry functions
- `cmd_*`: Command handler functions
- `*_t`: Type definitions
- `g_*`: Global variables
- `UPPER_CASE`: Macros and enums
- `lower_case`: Functions, variables, structs

## Adding New Commands

Commands are defined in XML files (`commands.xml`) within each module directory. The XML structure uses:

1. **Elements** - Define keywords and parameters:
```xml
<element id="1" type="keyword">
    <name>show</name>
    <description>Display information</description>
</element>
<element id="2" type="parameter">
    <name>&lt;hostname&gt;</name>
    <type>string(1-63)</type>
    <description>System hostname</description>
</element>
```

2. **Commands** - Combine elements into expressions:
```xml
<command>
    <expression>1 2</expression>  <!-- References element IDs -->
    <views>3</views>               <!-- View ID where command is available -->
    <view-id>4</view-id>          <!-- Optional: view to switch to after execution -->
</command>
```

The last element in each expression is automatically marked as an `is_end_node`. For commands that should execute at intermediate points (e.g., "show bgp" and "show bgp peer" both valid), create separate command entries with different expression lengths.

## Code Style

- C11, LLVM-based formatting with Allman braces
- 4-space indent, 120-char line limit
- Right-aligned pointers (`char *ptr`)
- Braces required for all control statements
