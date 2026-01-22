#!/bin/bash
# Check code formatting without modifying files

echo "Checking code formatting..."

FAILED=0

# Find and check all C source files
find src include -name "*.c" -o -name "*.h" | while read file; do
    if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
        echo "❌ Format issues in: $file"
        FAILED=1
    else
        echo "✓ $file"
    fi
done

if [ $FAILED -eq 1 ]; then
    echo ""
    echo "Format check failed! Run ./scripts/format-code.sh to fix."
    exit 1
else
    echo ""
    echo "All files are properly formatted!"
    exit 0
fi
