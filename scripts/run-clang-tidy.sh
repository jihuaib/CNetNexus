#!/bin/bash
# Run clang-tidy on all C source files

echo "Running clang-tidy analysis..."

# Get libxml2 include path
LIBXML2_INCLUDE=$(pkg-config --cflags-only-I libxml-2.0 2>/dev/null || echo "-I/usr/include/libxml2")

# Find all C source files
find src -name "*.c" -o -name "*.h" | while read file; do
    echo "Analyzing: $file"
    clang-tidy "$file" -- -Iinclude -Isrc $LIBXML2_INCLUDE
done

echo "Analysis complete!"
