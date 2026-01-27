# NetNexus Development Guide

Quick reference for NetNexus development.

## Quick Start

```bash
# 1. Build
./scripts/dev/build.sh

# 2. Run
./scripts/dev/start.sh

# 3. Connect
telnet localhost 3788
```

## Development Commands

### Build
```bash
./scripts/dev/build.sh              # Debug build
./scripts/dev/build.sh --release    # Release build
./scripts/dev/build.sh --clean      # Clean + rebuild
./scripts/dev/build.sh -j 8         # 8 parallel jobs
```

### Run
```bash
./scripts/dev/start.sh              # Normal start
./scripts/dev/debug.sh              # Start with gdb
```

### Clean
```bash
./scripts/dev/clean.sh              # Clean build only
./scripts/dev/clean.sh --data       # Also clean data
./scripts/dev/clean.sh --all        # Clean everything
```

## VSCode Shortcuts

- `Ctrl+Shift+B` - Build
- `F5` - Start debugging
- `Ctrl+F5` - Run without debugging
- `F9` - Toggle breakpoint
- `F10` - Step over
- `F11` - Step into

## Project Structure

```
NetNexus/
├── src/              # Source code
│   ├── cfg/         # CLI framework
│   ├── db/          # Database module
│   ├── bgp/         # BGP module
│   ├── dev/         # Dev module
│   └── utils/       # Utilities
├── include/          # Public headers
├── build/            # Build output
│   ├── bin/         # Executables
│   └── lib/         # Libraries
├── data/             # Development databases
├── scripts/          # Dev/deployment scripts
└── .vscode/          # VSCode config
```

## Common Tasks

### Add New Module

1. **Create module directory:**
   ```bash
   mkdir -p src/mymodule
   ```

2. **Create main file** (`src/mymodule/nn_mymodule_main.c`):
   ```c
   #include "nn_dev.h"
   #include "nn_errcode.h"

   static int32_t mymodule_init(void *module) {
       printf("[mymodule] Initializing\n");
       return NN_ERRCODE_SUCCESS;
   }

   static void mymodule_cleanup(void) {
       printf("[mymodule] Cleaning up\n");
   }

   static void __attribute__((constructor)) register_mymodule(void) {
       nn_dev_register_module(0x00000005, "nn_mymodule",
                              mymodule_init, mymodule_cleanup);
   }
   ```

3. **Create CMakeLists.txt:**
   ```cmake
   add_library(nn_mymodule SHARED nn_mymodule_main.c)
   target_include_directories(nn_mymodule PRIVATE ${PROJECT_SOURCE_DIR}/include)
   install(TARGETS nn_mymodule LIBRARY DESTINATION lib)
   ```

4. **Create commands.xml:**
   ```xml
   <?xml version="1.0" encoding="UTF-8"?>
   <configuration module-id="5">
       <views></views>
       <command_groups></command_groups>
   </configuration>
   ```

5. **Update src/CMakeLists.txt:**
   ```cmake
   add_subdirectory(mymodule)
   ```

6. **Link in main binary:**
   ```cmake
   target_link_libraries(netnexus PRIVATE nn_mymodule)
   ```

### Add New CLI Command

1. **Edit module's commands.xml:**
   ```xml
   <element cfg-id="1" type="keyword">
       <name>mycommand</name>
       <description>My custom command</description>
   </element>

   <command>
       <expression>1</expression>
       <views>3</views>  <!-- USER view -->
   </command>
   ```

2. **Implement handler in module code**

3. **Rebuild and test:**
   ```bash
   ./scripts/dev/build.sh
   ./scripts/dev/start.sh
   ```

### Debug a Crash

1. **Build with debug symbols:**
   ```bash
   ./scripts/dev/build.sh
   ```

2. **Run with gdb:**
   ```bash
   ./scripts/dev/debug.sh
   ```

3. **In GDB:**
   ```gdb
   (gdb) break main
   (gdb) run
   (gdb) continue
   # ... crash occurs ...
   (gdb) backtrace
   (gdb) info locals
   (gdb) print variable_name
   ```

### Check Memory Leaks

```bash
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./build/bin/netnexus
```

### Profile Performance

```bash
# Install perf
sudo apt install linux-tools-generic

# Record
perf record -g ./build/bin/netnexus

# Analyze
perf report
```

## Database Development

### View Database Schema

```bash
sqlite3 data/bgp/bgp_db.db ".schema"
```

### Query Database

```bash
sqlite3 data/bgp/bgp_db.db "SELECT * FROM bgp_protocol;"
```

### Reset Database

```bash
rm -rf data/
./scripts/dev/start.sh  # Will recreate from XML
```

### Add Database Table

1. **Edit module's commands.xml:**
   ```xml
   <dbs>
       <db db-name="mymodule_db">
           <tables>
               <table table-name="my_table">
                   <fields>
                       <field field-name="my_field" type="uint(0-65535)"/>
                   </fields>
               </table>
           </tables>
       </db>
   </dbs>
   ```

2. **Use in code:**
   ```c
   #include "nn_db.h"

   nn_db_value_t values[1];
   values[0] = nn_db_value_int(12345);
   const char *fields[1] = {"my_field"};

   nn_db_insert("mymodule_db", "my_table", fields, values, 1);
   ```

## Testing

### Manual Testing

```bash
# Terminal 1: Run server
./scripts/dev/start.sh

# Terminal 2: Connect
telnet localhost 3788

# Test commands
netnexus> help
netnexus> show version
netnexus> config
netnexus(config)> ?
```

### Automated Testing

```bash
# Using expect script
cat > test.exp <<'EOF'
#!/usr/bin/expect
spawn telnet localhost 3788
expect "netnexus>"
send "help\r"
expect "netnexus>"
send "exit\r"
expect eof
EOF

chmod +x test.exp
./test.exp
```

## Debugging Tips

### Enable Core Dumps

```bash
ulimit -c unlimited
echo "core.%e.%p" | sudo tee /proc/sys/kernel/core_pattern
```

### Analyze Core Dump

```bash
gdb ./build/bin/netnexus core.*
(gdb) backtrace full
(gdb) info registers
(gdb) disassemble
```

### Attach to Running Process

```bash
# Terminal 1: Run
./scripts/dev/start.sh

# Terminal 2: Get PID
ps aux | grep netnexus

# Attach gdb
sudo gdb -p <PID>
(gdb) continue
# ... trigger issue ...
(gdb) backtrace
```

### Print Debug Messages

```c
// Add to code
#define DEBUG_PRINT(fmt, ...) \
    fprintf(stderr, "[DEBUG] %s:%d - " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__)

DEBUG_PRINT("Variable value: %d", my_var);
```

### Trace Function Calls

```bash
# Using ltrace
ltrace -f ./build/bin/netnexus

# Using strace
strace -f ./build/bin/netnexus
```

## Configuration

### XML Path Resolution

Priority order:
1. `$NN_RESOURCES_DIR/{module}/commands.xml`
2. `/opt/netnexus/resources/{module}/commands.xml`
3. `<exe_dir>/../../src/{module}/commands.xml` (development)
4. `../../src/{module}/commands.xml` (fallback)

### Database Path Resolution

- Development: `./data/{module}/{db_name}.db`
- Production: `/opt/netnexus/data/{module}/{db_name}.db`

## Troubleshooting

### Build Errors

```bash
# Clean build
./scripts/dev/clean.sh
./scripts/dev/build.sh --verbose

# Check dependencies
pkg-config --modversion glib-2.0 libxml-2.0 sqlite3
```

### Runtime Errors

```bash
# Check library path
ldd ./build/bin/netnexus

# Run with verbose
LD_DEBUG=libs ./scripts/dev/start.sh

# Check config files
ls -la src/*/commands.xml
```

### Port Already in Use

```bash
# Find process using port 3788
sudo lsof -i :3788

# Kill process
sudo kill <PID>

# Or use different port (modify code)
```

### Permission Denied

```bash
# Check permissions
ls -la build/bin/netnexus

# Make executable
chmod +x build/bin/netnexus
```

## Resources

- [CLAUDE.md](CLAUDE.md) - Full architecture guide
- [DEPLOYMENT.md](DEPLOYMENT.md) - Deployment guide
- [scripts/README.md](scripts/README.md) - Script documentation

## Getting Help

1. Check build output: `./scripts/dev/build.sh --verbose`
2. Run with debugger: `./scripts/dev/debug.sh`
3. Check logs when running
4. Review architecture: [CLAUDE.md](CLAUDE.md)
