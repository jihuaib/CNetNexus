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
    echo "[1/6] Cleaning existing installation..."
    rm -rf "${INSTALL_DIR}"
fi

# Create installation directory
echo "[2/6] Creating installation directory..."
mkdir -p "${INSTALL_DIR}"

# Install all files
echo "[3/6] Installing files..."
cp -r "${PACKAGE_DIR}/bin" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/lib" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/resources" "${INSTALL_DIR}/"
cp -r "${PACKAGE_DIR}/scripts" "${INSTALL_DIR}/"
[ -f "${PACKAGE_DIR}/VERSION" ] && cp "${PACKAGE_DIR}/VERSION" "${INSTALL_DIR}/"
[ -f "${PACKAGE_DIR}/README.txt" ] && cp "${PACKAGE_DIR}/README.txt" "${INSTALL_DIR}/"

# Set permissions
echo "[4/6] Setting permissions..."
chmod +x "${INSTALL_DIR}/bin"/*
chmod +x "${INSTALL_DIR}/scripts"/*.sh

# Configure host MPLS modules to load at boot when this script runs on a Linux host.
echo "[5/6] Configuring MPLS kernel module autoload..."
if [ -d /etc/modules-load.d ] && [ -w /etc/modules-load.d ]; then
    cat > /etc/modules-load.d/netnexus-mpls.conf << 'EOF'
mpls_router
mpls_iptunnel
mpls_gso
EOF
    echo "  - Installed /etc/modules-load.d/netnexus-mpls.conf"
else
    echo "  - WARN: cannot write /etc/modules-load.d; run deploy with sudo to enable MPLS module autoload"
fi

if command -v modprobe >/dev/null 2>&1; then
    for mod in mpls_router mpls_iptunnel mpls_gso; do
        modprobe "${mod}" 2>/dev/null || echo "  - WARN: failed to load ${mod}; MPLS forwarding may not work until it is loaded"
    done
else
    echo "  - WARN: modprobe not available; skipped immediate MPLS module load"
fi

# Create data directory
echo "[6/6] Creating data directory..."
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
