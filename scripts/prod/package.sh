#!/bin/bash
#
# NetNexus Package Script
# Creates a deployment package with binaries and configuration files
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
PACKAGE_DIR="${PROJECT_ROOT}/package"
VERSION="${VERSION:-1.0.0}"
PACKAGE_NAME="netnexus-${VERSION}"

echo "==================================="
echo "NetNexus Package Builder"
echo "==================================="
echo "Version: ${VERSION}"
echo "Build dir: ${BUILD_DIR}"
echo "Package dir: ${PACKAGE_DIR}"
echo ""

# Clean previous package
if [ -d "${PACKAGE_DIR}" ]; then
    echo "[1/6] Cleaning previous package..."
    rm -rf "${PACKAGE_DIR}"
fi

# Create package structure
echo "[2/6] Creating package structure..."
mkdir -p "${PACKAGE_DIR}/${PACKAGE_NAME}/bin"
mkdir -p "${PACKAGE_DIR}/${PACKAGE_NAME}/lib"
mkdir -p "${PACKAGE_DIR}/${PACKAGE_NAME}/resources"
mkdir -p "${PACKAGE_DIR}/${PACKAGE_NAME}/scripts"

# Copy binaries
echo "[3/6] Copying binaries..."
if [ ! -f "${BUILD_DIR}/bin/netnexus" ]; then
    echo "Error: Build not found. Please run 'cmake --build build' first."
    exit 1
fi

cp "${BUILD_DIR}/bin/netnexus" "${PACKAGE_DIR}/${PACKAGE_NAME}/bin/"
chmod +x "${PACKAGE_DIR}/${PACKAGE_NAME}/bin/netnexus"

# Copy libraries
echo "[4/6] Copying libraries..."
cp "${BUILD_DIR}/lib"/libnn_*.so* "${PACKAGE_DIR}/${PACKAGE_NAME}/lib/" 2>/dev/null || true

# Copy configuration files (XML files from src)
echo "[5/6] Copying configuration files..."
for module_dir in "${PROJECT_ROOT}/src"/*; do
    if [ -d "$module_dir" ]; then
        module_name=$(basename "$module_dir")
        xml_file="${module_dir}/commands.xml"

        if [ -f "$xml_file" ]; then
            mkdir -p "${PACKAGE_DIR}/${PACKAGE_NAME}/resources/${module_name}"
            cp "$xml_file" "${PACKAGE_DIR}/${PACKAGE_NAME}/resources/${module_name}/"
            echo "  - Copied ${module_name}/commands.xml"
        fi
    fi
done

# Copy deployment scripts
echo "[6/6] Copying deployment scripts..."
cp "${SCRIPT_DIR}/deploy.sh" "${PACKAGE_DIR}/${PACKAGE_NAME}/scripts/"
cp "${SCRIPT_DIR}/start.sh" "${PACKAGE_DIR}/${PACKAGE_NAME}/scripts/"
chmod +x "${PACKAGE_DIR}/${PACKAGE_NAME}/scripts"/*.sh

# Create version file
echo "${VERSION}" > "${PACKAGE_DIR}/${PACKAGE_NAME}/VERSION"

# Create README
cat > "${PACKAGE_DIR}/${PACKAGE_NAME}/README.txt" << 'EOF'
NetNexus Deployment Package
===========================

Installation:
1. Extract this package to a temporary directory
2. Run: sudo ./scripts/deploy.sh
3. Start service: sudo systemctl start netnexus

Manual start:
  sudo /opt/netnexus/bin/start.sh

Configuration files:
  /opt/netnexus/resources/

Logs:
  Check with: sudo journalctl -u netnexus -f

EOF

# Create tarball
echo ""
echo "Creating tarball..."
cd "${PACKAGE_DIR}"
tar czf "${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"

echo ""
echo "==================================="
echo "Package created successfully!"
echo "==================================="
echo "Package: ${PACKAGE_DIR}/${PACKAGE_NAME}.tar.gz"
echo "Size: $(du -h "${PACKAGE_NAME}.tar.gz" | cut -f1)"
echo ""
echo "To deploy:"
echo "  tar xzf ${PACKAGE_NAME}.tar.gz"
echo "  cd ${PACKAGE_NAME}"
echo "  sudo ./scripts/deploy.sh"
echo ""
