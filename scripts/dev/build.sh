#!/bin/bash
#
# NetNexus Development Build Script
# Quick build with optional clean and parallel compilation
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"

# Parse arguments
CLEAN=0
VERBOSE=0
JOBS=$(nproc)
BUILD_TYPE="Debug"

while [[ $# -gt 0 ]]; do
    case $1 in
        -c|--clean)
            CLEAN=1
            shift
            ;;
        -v|--verbose)
            VERBOSE=1
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -r|--release)
            BUILD_TYPE="Release"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -c, --clean      Clean build (remove build directory)"
            echo "  -v, --verbose    Verbose output"
            echo "  -j, --jobs N     Number of parallel jobs (default: $(nproc))"
            echo "  -r, --release    Release build (default: Debug)"
            echo "  -h, --help       Show this help"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

echo "==================================="
echo "NetNexus Development Build"
echo "==================================="
echo "Build type: ${BUILD_TYPE}"
echo "Jobs: ${JOBS}"
echo ""

# Clean build if requested
if [ $CLEAN -eq 1 ]; then
    echo "[1/3] Cleaning build directory..."
    rm -rf "${BUILD_DIR}"
    echo "  Clean complete"
fi

# Create build directory
if [ ! -d "${BUILD_DIR}" ]; then
    echo "[1/3] Creating build directory..."
    mkdir -p "${BUILD_DIR}"
fi

# Configure with CMake
echo "[2/3] Configuring with CMake..."
cd "${BUILD_DIR}"

CMAKE_ARGS="-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
if [ $VERBOSE -eq 1 ]; then
    CMAKE_ARGS="${CMAKE_ARGS} -DCMAKE_VERBOSE_MAKEFILE=ON"
fi

cmake .. ${CMAKE_ARGS}

# Build
echo "[3/3] Building..."
START_TIME=$(date +%s)

if [ $VERBOSE -eq 1 ]; then
    make -j${JOBS} VERBOSE=1
else
    make -j${JOBS}
fi

END_TIME=$(date +%s)
BUILD_TIME=$((END_TIME - START_TIME))

echo ""
echo "==================================="
echo "Build complete!"
echo "==================================="
echo "Build time: ${BUILD_TIME}s"
echo "Binary: ${BUILD_DIR}/bin/netnexus"
echo ""
echo "To run:"
echo "  ./scripts/dev/start.sh"
echo ""
echo "To debug:"
echo "  ./scripts/dev/debug.sh"
echo ""
