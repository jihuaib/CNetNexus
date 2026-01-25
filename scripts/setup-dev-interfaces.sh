#!/bin/bash
# Setup development network interfaces for NetNexus testing

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "======================================"
echo "NetNexus Development Interface Setup"
echo "======================================"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Please run with sudo${NC}"
    exit 1
fi

# Number of interfaces to create
NUM_INTERFACES=4

echo "Creating ${NUM_INTERFACES} virtual network interfaces..."
echo ""

# Create veth pairs
for i in $(seq 0 $((NUM_INTERFACES - 1))); do
    IFACE="veth${i}"
    PEER="veth${i}-peer"
    
    # Check if interface already exists
    if ip link show ${IFACE} &> /dev/null; then
        echo -e "${YELLOW}⚠ ${IFACE} already exists, skipping${NC}"
        continue
    fi
    
    # Create veth pair
    ip link add ${IFACE} type veth peer name ${PEER}
    
    # Bring up interfaces (no IP configuration)
    ip link set ${IFACE} up
    ip link set ${PEER} up
    
    echo -e "${GREEN}✓ Created ${IFACE} <-> ${PEER}${NC}"
    echo "  Status: UP (no IP configured)"
done

echo ""
echo "======================================"
echo "Setup Complete!"
echo "======================================"
echo ""
echo "Available interfaces:"
ip link show | grep veth | awk '{print "  - " $2}' | sed 's/:$//' | sed 's/@.*//'
echo ""
echo "Next steps:"
echo "1. Run NetNexus: ./build/bin/netnexus"
echo "2. Connect via telnet: telnet localhost 3788"
echo "3. Configure interfaces via CLI:"
echo "   interface veth0"
echo "   ip address 192.168.10.1 255.255.255.0"
echo "   no shutdown"
echo ""
echo "To cleanup: sudo ./scripts/cleanup-dev-interfaces.sh"
echo ""
