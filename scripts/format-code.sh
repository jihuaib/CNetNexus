#!/bin/bash
# Format all C source and header files using clang-format

echo "Formatting C source files..."

# Find all .c and .h files and format them
find src include -name '*.c' -o -name '*.h' | while read -r file; do
    echo "Formatting: $file"
    clang-format -i "$file"
done

echo "Done! All files have been formatted."
