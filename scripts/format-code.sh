#!/bin/bash
# Format all C source files with clang-format

echo "Formatting C source files..."

# Find and format all C source files
find src include -name "*.c" -o -name "*.h" | while read file; do
    echo "Formatting: $file"
    clang-format -i "$file"
done

echo "Formatting complete!"
