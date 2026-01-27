#!/bin/bash
# Build NetNexus with AddressSanitizer enabled
#
# Usage: ./scripts/build-with-asan.sh
#
# This script builds NetNexus with AddressSanitizer (ASan) for fast memory
# leak detection during development. The binary will be placed in build-asan/bin/

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Building NetNexus with AddressSanitizer...${NC}"
echo ""

# Create build directory
BUILD_DIR="build-asan"
mkdir -p "$BUILD_DIR"

# Configure with ASan enabled
echo "Configuring CMake with AddressSanitizer..."
cmake -B "$BUILD_DIR" \
      -DCMAKE_BUILD_TYPE=Debug \
      -DENABLE_ASAN=ON

echo ""
echo "Building..."
cmake --build "$BUILD_DIR"

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "Binary location: $BUILD_DIR/bin/netnexus"
echo ""
echo "To run with AddressSanitizer:"
echo "  ./$BUILD_DIR/bin/netnexus"
echo ""
echo "AddressSanitizer will automatically report memory leaks on exit."
echo ""
echo -e "${YELLOW}Note:${NC} Set ASAN_OPTIONS environment variable for more control:"
echo "  export ASAN_OPTIONS=detect_leaks=1:log_path=asan.log"
echo "  ./$BUILD_DIR/bin/netnexus"
