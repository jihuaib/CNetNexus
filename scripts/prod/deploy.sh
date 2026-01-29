#!/bin/bash
#
# NetNexus Deployment Script
# Installs NetNexus to /opt/netnexus
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_DIR="${INSTALL_DIR:-/opt/netnexus}"

echo "==================================="
echo "NetNexus Deployment"
echo "==================================="
echo "Package: ${PACKAGE_DIR}"
echo "Install: ${INSTALL_DIR}"
echo ""

# Clean existing installation
if [ -d "${INSTALL_DIR}" ]; then
    echo "[1/5] Cleaning existing installation..."
    rm -rf "${INSTALL_DIR}"
fi

# Create installation directory
echo "[2/5] Creating installation directory..."
mkdir -p "${INSTALL_DIR}"

# Install all files
echo "[3/5] Installing files..."
cp -r "${PACKAGE_DIR}/bin" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/lib" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/resources" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/scripts" "${INSTALL_DIR}/"
[ -f "${PACKAGE_DIR}/VERSION" ] && cp "${PACKAGE_DIR}/VERSION" "${INSTALL_DIR}/"
[ -f "${PACKAGE_DIR}/README.txt" ] && cp "${PACKAGE_DIR}/README.txt" "${INSTALL_DIR}/"

# Set permissions
echo "[4/5] Setting permissions..."
chmod +x "${INSTALL_DIR}/bin"/*
chmod +x "${INSTALL_DIR}/scripts"/*.sh

# Create data directory
echo "[5/5] Creating data directory..."
mkdir -p "${INSTALL_DIR}/data"

echo ""
echo "==================================="
echo "Deployment complete!"
echo "==================================="
echo ""
echo "Installation: ${INSTALL_DIR}"
echo "Version: $(cat ${INSTALL_DIR}/VERSION 2>/dev/null || echo 'unknown')"
echo ""
echo "To start NetNexus:"
echo "  ${INSTALL_DIR}/scripts/start.sh"
echo ""
