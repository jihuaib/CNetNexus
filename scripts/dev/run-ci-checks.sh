#!/bin/bash
# Run all code quality checks locally (same as GitHub Actions)
# This script runs the same checks as the CI pipeline

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

echo "======================================"
echo "Running Code Quality Checks"
echo "======================================"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

FAILED=0

# ======================================
# 1. Check Code Formatting (clang-format)
# ======================================
echo "1. Checking code formatting with clang-format..."
echo "--------------------------------------"

if ! command -v clang-format &> /dev/null; then
    echo -e "${RED}❌ clang-format not found. Please install it:${NC}"
    echo "   sudo apt-get install clang-format"
    FAILED=1
else
    FORMAT_FAILED=0
    while IFS= read -r file; do
        if ! clang-format --dry-run --Werror "$file" 2>/dev/null; then
            echo -e "${RED}❌ Format issues in: $file${NC}"
            FORMAT_FAILED=1
        fi
    done < <(find "$PROJECT_ROOT/src" "$PROJECT_ROOT/include" -name '*.c' -o -name '*.h')
    
    if [ $FORMAT_FAILED -eq 1 ]; then
        echo -e "${RED}❌ Code formatting check FAILED${NC}"
        echo "   Run: ./scripts/format-code.sh to fix"
        FAILED=1
    else
        echo -e "${GREEN}✓ Code formatting check PASSED${NC}"
    fi
fi

echo ""

# ======================================
# 2. Static Analysis (clang-tidy) - Debug
# ======================================
echo "2. Running static analysis with clang-tidy (Debug)..."
echo "--------------------------------------"

if ! command -v clang-tidy &> /dev/null; then
    echo -e "${RED}❌ clang-tidy not found. Please install it:${NC}"
    echo "   sudo apt-get install clang-tidy"
    FAILED=1
else
    # Check if build directory exists
    if [ ! -d "$PROJECT_ROOT/build" ]; then
        echo -e "${YELLOW}⚠ Build directory not found. Creating...${NC}"
        cd "$PROJECT_ROOT"
        cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    fi
    
    # Check if compile_commands.json exists
    if [ ! -f "$PROJECT_ROOT/build/compile_commands.json" ]; then
        echo -e "${YELLOW}⚠ compile_commands.json not found. Regenerating...${NC}"
        cd "$PROJECT_ROOT"
        cmake -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    fi
    
    TIDY_OUTPUT=$(mktemp)
    TIDY_ERRORS=$(mktemp)
    TIDY_FAILED=0
    
    echo "Running clang-tidy on source files..."
    
    # Run clang-tidy on all source and header files
    find "$PROJECT_ROOT/src" "$PROJECT_ROOT/include" \( -name '*.c' -o -name '*.h' \) | while read -r file; do
        echo "  Analyzing: $file"
        clang-tidy -p "$PROJECT_ROOT/build" "$file" 2>&1 | tee -a "$TIDY_OUTPUT"
    done
    
    # Extract errors and warnings
    grep "error:\|warning:" "$TIDY_OUTPUT" > "$TIDY_ERRORS" || true
    
    # Display summary
    ERROR_COUNT=$(grep -c "error:" "$TIDY_ERRORS" || echo "0")
    WARNING_COUNT=$(grep -c "warning:" "$TIDY_ERRORS" || echo "0")
    
    echo ""
    echo "Analysis Results:"
    echo "  Errors: $ERROR_COUNT"
    echo "  Warnings: $WARNING_COUNT"
    
    if [ "$ERROR_COUNT" -gt 0 ]; then
        echo ""
        echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${RED}Errors Found:${NC}"
        echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        grep "error:" "$TIDY_ERRORS" | head -20
        if [ "$ERROR_COUNT" -gt 20 ]; then
            echo -e "${YELLOW}... and $((ERROR_COUNT - 20)) more errors${NC}"
        fi
        echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${RED}❌ Static analysis FAILED (errors found)${NC}"
        FAILED=1
    elif [ "$WARNING_COUNT" -gt 0 ]; then
        echo ""
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${YELLOW}Warnings Found:${NC}"
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        grep "warning:" "$TIDY_ERRORS" | head -20
        if [ "$WARNING_COUNT" -gt 20 ]; then
            echo -e "${YELLOW}... and $((WARNING_COUNT - 20)) more warnings${NC}"
        fi
        echo -e "${YELLOW}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
        echo -e "${YELLOW}⚠ Static analysis completed with warnings${NC}"
    else
        echo -e "${GREEN}✓ Static analysis PASSED${NC}"
    fi
    
    rm -f "$TIDY_OUTPUT" "$TIDY_ERRORS"
fi

echo ""

# ======================================
# 3. Build Test (Debug)
# ======================================
echo "3. Building project (Debug)..."
echo "--------------------------------------"

cd "$PROJECT_ROOT"
BUILD_LOG=$(mktemp)

if cmake --build build --config Debug 2>&1 | tee "$BUILD_LOG"; then
    echo -e "${GREEN}✓ Build PASSED (Debug)${NC}"
else
    echo ""
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}Build Errors (Debug):${NC}"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    grep "error:" "$BUILD_LOG" | head -20 || tail -30 "$BUILD_LOG"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}❌ Build FAILED (Debug)${NC}"
    FAILED=1
fi

rm -f "$BUILD_LOG"

echo ""

# ======================================
# 4. Build Test (Release)
# ======================================
echo "4. Building project (Release)..."
echo "--------------------------------------"

cd "$PROJECT_ROOT"
if [ ! -d "$PROJECT_ROOT/build-release" ]; then
    cmake -B build-release -DCMAKE_BUILD_TYPE=Release
fi

BUILD_LOG=$(mktemp)

if cmake --build build-release --config Release 2>&1 | tee "$BUILD_LOG"; then
    echo -e "${GREEN}✓ Build PASSED (Release)${NC}"
else
    echo ""
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}Build Errors (Release):${NC}"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    grep "error:" "$BUILD_LOG" | head -20 || tail -30 "$BUILD_LOG"
    echo -e "${RED}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${RED}❌ Build FAILED (Release)${NC}"
    FAILED=1
fi

rm -f "$BUILD_LOG"

echo ""

# ======================================
# Summary
# ======================================
echo "======================================"
echo "Summary"
echo "======================================"

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}✓ All checks PASSED!${NC}"
    echo ""
    echo "Your code is ready to push!"
    exit 0
else
    echo -e "${RED}❌ Some checks FAILED!${NC}"
    echo ""
    echo "Please fix the issues above before pushing."
    exit 1
fi
