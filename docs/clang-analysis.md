# Clang Code Analysis Setup

## Overview

Clang code analysis tools have been configured for the NetNexus project:
- **clang-tidy**: Static code analysis
- **clang-format**: Code formatting

## Files Created

### Configuration Files
- [.clang-tidy](file:///home/jhb/NetNexus/.clang-tidy) - Static analysis rules
- [.clang-format](file:///home/jhb/NetNexus/.clang-format) - Code formatting style

### Scripts
- [scripts/run-clang-tidy.sh](file:///home/jhb/NetNexus/scripts/run-clang-tidy.sh) - Run static analysis
- [scripts/format-code.sh](file:///home/jhb/NetNexus/scripts/format-code.sh) - Format all code

## Usage

### Run Static Analysis
```bash
./scripts/run-clang-tidy.sh
```

### Format Code
```bash
./scripts/format-code.sh
```

### Manual Commands
```bash
# Analyze a single file
clang-tidy src/cli/nn_cli_handler.c -- -Iinclude -Isrc

# Format a single file
clang-format -i src/cli/nn_cli_handler.c

# Check formatting without modifying
clang-format --dry-run --Werror src/cli/nn_cli_handler.c
```

## Clang-Tidy Checks

Enabled check categories:
- `bugprone-*` - Bug-prone code patterns
- `clang-analyzer-*` - Deep static analysis
- `performance-*` - Performance issues
- `portability-*` - Portability issues
- `readability-*` - Code readability
- `modernize-*` - Modern C practices
- `cppcoreguidelines-*` - C++ Core Guidelines
- `misc-*` - Miscellaneous checks

## Clang-Format Style

Based on LLVM style with customizations:
- **Indent**: 4 spaces
- **Column limit**: 100 characters
- **Braces**: Linux style (function braces on new line)
- **Pointer alignment**: Right (`uint32_t *ptr`)

## Integration with CMake

To integrate with CMake build:
```cmake
# Add to CMakeLists.txt
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
```

This generates `compile_commands.json` for better clang-tidy analysis.

## CI/CD Integration

Add to your CI pipeline:
```yaml
- name: Run clang-tidy
  run: ./scripts/run-clang-tidy.sh

- name: Check formatting
  run: |
    ./scripts/format-code.sh
    git diff --exit-code
```
