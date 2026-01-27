# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt install build-essential cmake libxml2-dev libsqlite3-dev pkg-config

# Development dependencies (optional)
sudo apt install gdb inotify-tools  # For debugging and file watching

# Build
mkdir build && cd build && cmake .. && make

# Build and run
cmake --build . --target run

# Clean
rm -rf build
```

## Development Workflow

### Quick Start

```bash
# 1. Build
./scripts/dev/build.sh

# 2. Run
./scripts/dev/start.sh

# 3. Connect
telnet localhost 3788
```

### Development Scripts

All development scripts are in `scripts/` directory:

**Build and Clean:**
```bash
./scripts/dev/build.sh              # Quick build (Debug mode)
./scripts/dev/build.sh --release    # Release build
./scripts/dev/build.sh --clean      # Clean + rebuild
./scripts/dev/build.sh -j 8         # Build with 8 parallel jobs

./scripts/dev/clean.sh              # Clean build directory only
./scripts/dev/clean.sh --data       # Also clean data directory
./scripts/dev/clean.sh --all        # Clean everything
```

**Run and Debug:**
```bash
./scripts/dev/start.sh              # Start in development mode
./scripts/dev/debug.sh              # Start with gdb debugger
```

**Example GDB Debug Session:**
```bash
./scripts/dev/debug.sh

# In GDB:
(gdb) break main                    # Set breakpoint
(gdb) run                           # Start program
(gdb) continue                      # Continue execution
(gdb) backtrace                     # Show call stack
(gdb) print variable_name           # Inspect variable
(gdb) quit                          # Exit
```

### VSCode Integration

Open the project in VSCode with full debugging support:

**Build Tasks (Ctrl+Shift+B):**
- Build Debug (default)
- Build Release
- Clean
- Rebuild (clean + build)
- Watch (auto-rebuild on changes)

**Debug Configurations (F5):**
- Debug NetNexus - Build and debug
- Attach to NetNexus - Attach to running process
- Run NetNexus (No Debug) - Run without debugging

**Keyboard Shortcuts:**
- `Ctrl+Shift+B` - Build
- `F5` - Start debugging
- `Ctrl+F5` - Run without debugging
- `Shift+F5` - Stop debugging
- `F9` - Toggle breakpoint
- `F10` - Step over
- `F11` - Step into

### Development Environment

**Directory Structure:**
```
NetNexus/
├── src/              # Source code (auto-detected for XML configs)
├── include/          # Public headers
├── build/            # Build output
│   ├── bin/         # Executables
│   └── lib/         # Libraries
├── data/             # Development database storage
├── scripts/          # Development and deployment scripts
└── .vscode/          # VSCode configuration
```

**Environment Variables:**
- `LD_LIBRARY_PATH` - Automatically set to `build/lib/`
- `NN_RESOURCES_DIR` - Not needed in dev (auto-discovered from `src/`)

**Configuration Files:**
Config XML files are automatically discovered from source directories:
- `src/cfg/commands.xml`
- `src/dev/commands.xml`
- `src/bgp/commands.xml`
- `src/db/commands.xml`

**Database Files:**
Development databases stored in `data/`:
```
data/
└── bgp/
    └── bgp_db.db
```

When you save a `.c`, `.h`, or `.xml` file, the watch script automatically:
1. Detects the change
2. Rebuilds the project
3. Shows build results

Restart NetNexus (Terminal 2) to see your changes.

### Debugging Tips

**Memory Leaks:**
```bash
# Build with AddressSanitizer
cd build
cmake -DCMAKE_C_FLAGS="-fsanitize=address -g" ..
make
./bin/netnexus
```

**Valgrind:**
```bash
valgrind --leak-check=full --show-leak-kinds=all \
  ./build/bin/netnexus
```

**Core Dumps:**
```bash
# Enable core dumps
ulimit -c unlimited

# Run and crash
./build/bin/netnexus

# Analyze core dump
gdb ./build/bin/netnexus core
```

**Verbose Logging:**
Add debug prints or use gdb to trace execution:
```c
printf("[DEBUG] %s:%d - Variable: %d\n", __FILE__, __LINE__, var);
```

### Common Development Tasks

**Add a New Module:**
1. Create `src/mymodule/` directory
2. Add `nn_mymodule_main.c` with constructor
3. Add `commands.xml` with CLI commands
4. Add `CMakeLists.txt`
5. Update `src/CMakeLists.txt` to include new module
6. Build and test

**Add a New CLI Command:**
1. Edit `src/{module}/commands.xml`
2. Add element definition
3. Add command expression
4. Add command handler in module code
5. Rebuild and test

**Debug a Crash:**
1. Build with debug symbols: `./scripts/dev/build.sh`
2. Run with gdb: `./scripts/dev/debug.sh`
3. Set breakpoint: `break suspicious_function`
4. Run: `run`
5. Analyze: `backtrace`, `print`, `info locals`

**Test Database Changes:**
```bash
# View database
sqlite3 data/bgp/bgp_db.db ".schema"
sqlite3 data/bgp/bgp_db.db "SELECT * FROM bgp_protocol;"

# Reset database
rm -rf data/
./scripts/dev/start.sh  # Will recreate
```

### Performance Profiling

**Using perf:**
```bash
# Record
perf record -g ./build/bin/netnexus

# Analyze
perf report
```

**Using gprof:**
```bash
# Build with profiling
cd build
cmake -DCMAKE_C_FLAGS="-pg" ..
make

# Run
./bin/netnexus

# Analyze
gprof ./bin/netnexus gmon.out > analysis.txt
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
- `build/lib/libnn_db.so` - Database module (SQLite storage)
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
├── db/                         # Database module (libnn_db.so)
│   ├── nn_db_main.c/h          # Module lifecycle
│   ├── nn_db_registry.c/h      # DB definition storage
│   ├── nn_db_schema.c          # Schema management
│   ├── nn_db_api.c             # CRUD operations
│   └── commands.xml            # DB module config
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
│   └── libxml2, pthread, libnn_db
├── libnn_utils.so (utilities)
├── libnn_db.so (database module)
│   └── sqlite3
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

## Deployment

### Package Creation

Create a deployment package with all binaries and configuration files:

```bash
# Build the project first
mkdir build && cd build
cmake .. && make
cd ..

# Create deployment package
./scripts/package.sh

# Output: package/netnexus-1.0.0.tar.gz
```

The package includes:
- Binaries (`bin/netnexus`)
- Libraries (`lib/libnn_*.so`)
- Configuration files (`config/*/commands.xml`)
- Deployment scripts

### Production Deployment

Deploy to `/opt/netnexus`:

```bash
# Extract package
tar xzf netnexus-1.0.0.tar.gz
cd netnexus-1.0.0

# Deploy (requires sudo)
sudo ./scripts/deploy.sh
```

The deployment script:
- Installs to `/opt/netnexus`
- Copies config files to `/opt/netnexus/resources/`
- Preserves existing configs (creates `.bak` backups)
- Installs systemd service
- Sets up environment variables

### Service Management

```bash
# Start service
sudo systemctl start netnexus

# Enable on boot
sudo systemctl enable netnexus

# Check status
sudo systemctl status netnexus

# View logs
sudo journalctl -u netnexus -f

# Manual start
sudo /opt/netnexus/bin/start.sh
```

### Docker Deployment

Build and run with Docker:

```bash
# Build Docker image
docker build -t netnexus:latest .

# Run with docker-compose
docker-compose up -d

# View logs
docker-compose logs -f

# Stop
docker-compose down
```

Docker configuration:
- Exposed port: `3788` (telnet)
- Persistent data: `/opt/netnexus/data` (volume)
- Config directory: `/opt/netnexus/resources`
- Environment: `NN_RESOURCES_DIR=/opt/netnexus/resources`

### Configuration Path Resolution

The system resolves XML configuration files in this priority:

1. **Environment variable** `NN_RESOURCES_DIR`: `/opt/netnexus/resources/{module}/commands.xml`
2. **Production path**: `/opt/netnexus/resources/{module}/commands.xml`
3. **Development path**: `build/bin/../../src/{module}/commands.xml`
4. **Fallback**: `../../src/{module}/commands.xml`

### Database Storage

SQLite databases are stored in:
- **Development**: `./data/{module}/{db_name}.db`
- **Production**: `/opt/netnexus/data/{module}/{db_name}.db` (via environment)

Example: BGP module database at `/opt/netnexus/data/bgp/bgp_db.db`
