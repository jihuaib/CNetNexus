#!/bin/bash
# Run NetNexus with Valgrind memory leak detection
#
# Usage: ./scripts/check-memory-leaks.sh [additional netnexus arguments]
#
# This script runs NetNexus under Valgrind with comprehensive leak checking.
# Results are saved to valgrind-report.txt in the current directory.

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check if valgrind is installed
if ! command -v valgrind &> /dev/null; then
    echo -e "${RED}Error: Valgrind is not installed${NC}"
    echo "Install it with: sudo apt-get install valgrind"
    exit 1
fi

# Check if the binary exists
BINARY="./build/bin/netnexus"
if [ ! -f "$BINARY" ]; then
    echo -e "${RED}Error: NetNexus binary not found at $BINARY${NC}"
    echo "Build the project first with:"
    echo "  cmake -B build -DCMAKE_BUILD_TYPE=Debug"
    echo "  cmake --build build"
    exit 1
fi

# Check if suppression file exists
SUPP_FILE="./valgrind.supp"
if [ ! -f "$SUPP_FILE" ]; then
    echo -e "${YELLOW}Warning: Valgrind suppression file not found at $SUPP_FILE${NC}"
    echo "GLib false positives may be reported"
    SUPP_OPT=""
else
    SUPP_OPT="--suppressions=$SUPP_FILE"
fi

# Valgrind options
VALGRIND_OPTS="--leak-check=full \
               --show-leak-kinds=all \
               --track-origins=yes \
               --verbose \
               --log-file=valgrind-report.txt \
               $SUPP_OPT"

echo -e "${GREEN}Starting NetNexus with Valgrind...${NC}"
echo "Report will be saved to: valgrind-report.txt"
echo ""
echo "Press Ctrl+C to stop the server and generate the leak report"
echo ""

# Run valgrind
valgrind $VALGRIND_OPTS $BINARY "$@"

# Check if report was generated
if [ -f "valgrind-report.txt" ]; then
    echo ""
    echo -e "${GREEN}Valgrind report generated: valgrind-report.txt${NC}"
    echo ""
    echo "Summary:"
    grep -A 5 "LEAK SUMMARY" valgrind-report.txt || echo "No leak summary found"
    echo ""
    echo "View full report with: cat valgrind-report.txt"
else
    echo -e "${RED}Error: Valgrind report was not generated${NC}"
    exit 1
fi
