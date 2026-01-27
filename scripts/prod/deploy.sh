#!/bin/bash
#
# NetNexus Deployment Script
# Installs NetNexus to /opt/netnexus
#

set -e

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root (use sudo)"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
INSTALL_DIR="/opt/netnexus"
RESOURCE_DIR="${INSTALL_DIR}/resources"
BACKUP_DIR="${INSTALL_DIR}.backup.$(date +%Y%m%d_%H%M%S)"

echo "==================================="
echo "NetNexus Deployment"
echo "==================================="
echo "Package: ${PACKAGE_DIR}"
echo "Install: ${INSTALL_DIR}"
echo ""

# Backup existing installation
if [ -d "${INSTALL_DIR}" ]; then
    echo "[1/8] Backing up existing installation..."
    echo "  Backup: ${BACKUP_DIR}"

    # Stop service if running
    if systemctl is-active --quiet netnexus 2>/dev/null; then
        echo "  Stopping netnexus service..."
        systemctl stop netnexus
    fi

    # Create backup
    mv "${INSTALL_DIR}" "${BACKUP_DIR}"
    echo "  Backup complete"
else
    echo "[1/8] No existing installation found"
fi

# Create installation directory
echo "[2/8] Creating installation directory..."
mkdir -p "${INSTALL_DIR}"
mkdir -p "${RESOURCE_DIR}"

# Install binaries
echo "[3/8] Installing binaries..."
cp -r "${PACKAGE_DIR}/bin" "${INSTALL_DIR}/"
chmod +x "${INSTALL_DIR}/bin"/*

# Install libraries
echo "[4/8] Installing libraries..."
cp -r "${PACKAGE_DIR}/lib" "${INSTALL_DIR}/"

# Install configuration files
echo "[5/8] Installing configuration files..."
for module_dir in "${PACKAGE_DIR}/resources"/*; do
    if [ -d "$module_dir" ]; then
        module_name=$(basename "$module_dir")
        target_dir="${RESOURCE_DIR}/${module_name}"

        # Check for existing resources
        if [ -d "$target_dir" ] && [ -f "${target_dir}/commands.xml" ]; then
            # Backup exists, check if different
            if ! diff -q "${module_dir}/commands.xml" "${target_dir}/commands.xml" >/dev/null 2>&1; then
                echo "  - ${module_name}: Backing up existing resources"
                mv "${target_dir}/commands.xml" "${target_dir}/commands.xml.bak.$(date +%Y%m%d_%H%M%S)"
                cp "${module_dir}/commands.xml" "${target_dir}/"
            else
                echo "  - ${module_name}: Config unchanged"
            fi
        else
            # No existing resources, install new
            mkdir -p "${target_dir}"
            cp "${module_dir}/commands.xml" "${target_dir}/"
            echo "  - ${module_name}: Installed resources"
        fi
    fi
done

# Install startup scripts
echo "[6/8] Installing startup scripts..."
cp "${PACKAGE_DIR}/scripts/start.sh" "${INSTALL_DIR}/bin/"
chmod +x "${INSTALL_DIR}/bin/start.sh"

# Set permissions
echo "[7/8] Setting permissions..."
chown -R root:root "${INSTALL_DIR}"
chmod 755 "${INSTALL_DIR}"
chmod 755 "${INSTALL_DIR}/bin"
chmod 755 "${RESOURCE_DIR}"

# Create systemd service
echo "[8/8] Installing systemd service..."
cat > /etc/systemd/system/netnexus.service << 'EOF'
[Unit]
Description=NetNexus Network Management System
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=/opt/netnexus/bin
Environment="NN_RESOURCES_DIR=/opt/netnexus/resources"
ExecStart=/opt/netnexus/bin/netnexus
Restart=on-failure
RestartSec=5s
StandardOutput=journal
StandardError=journal

# Security settings
PrivateTmp=true
NoNewPrivileges=true

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd
systemctl daemon-reload

echo ""
echo "==================================="
echo "Deployment complete!"
echo "==================================="
echo ""
echo "Configuration directory: ${RESOURCE_DIR}"
echo ""
echo "To start NetNexus:"
echo "  sudo systemctl start netnexus"
echo ""
echo "To enable on boot:"
echo "  sudo systemctl enable netnexus"
echo ""
echo "To check status:"
echo "  sudo systemctl status netnexus"
echo ""
echo "To view logs:"
echo "  sudo journalctl -u netnexus -f"
echo ""
