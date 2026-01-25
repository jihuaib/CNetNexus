#!/bin/bash
# Cleanup development network interfaces

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "======================================"
echo "Cleaning up NetNexus Dev Interfaces"
echo "======================================"
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then 
    echo -e "${RED}Please run with sudo${NC}"
    exit 1
fi

# Number of interfaces
NUM_INTERFACES=4

echo "Removing virtual network interfaces..."
echo ""

REMOVED=0
for i in $(seq 0 $((NUM_INTERFACES - 1))); do
    IFACE="veth${i}"
    
    # Check if interface exists
    if ip link show ${IFACE} &> /dev/null; then
        ip link delete ${IFACE}
        echo -e "${GREEN}✓ Removed ${IFACE}${NC}"
        REMOVED=$((REMOVED + 1))
    else
        echo -e "${YELLOW}⚠ ${IFACE} not found, skipping${NC}"
    fi
done

echo ""
if [ $REMOVED -gt 0 ]; then
    echo -e "${GREEN}Cleanup complete! Removed ${REMOVED} interface(s).${NC}"
else
    echo -e "${YELLOW}No interfaces to remove.${NC}"
fi
echo ""
