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
echo "Removing loop interfaces (loop1..loopN)..."
echo ""

LOOP_REMOVED=0
while IFS= read -r LOOP_IFACE; do
    # Skip empty lines and protect system loopback "lo"
    if [ -z "${LOOP_IFACE}" ] || [ "${LOOP_IFACE}" = "lo" ]; then
        continue
    fi

    if ip link delete "${LOOP_IFACE}" &> /dev/null; then
        echo -e "${GREEN}✓ Removed ${LOOP_IFACE}${NC}"
        LOOP_REMOVED=$((LOOP_REMOVED + 1))
    else
        echo -e "${YELLOW}⚠ Failed to remove ${LOOP_IFACE}, skipping${NC}"
    fi
done < <(ip -o link show | awk -F': ' '{print $2}' | awk '/^loop[1-9][0-9]*$/')

echo ""
TOTAL_REMOVED=$((REMOVED + LOOP_REMOVED))
if [ $TOTAL_REMOVED -gt 0 ]; then
    echo -e "${GREEN}Cleanup complete! Removed ${TOTAL_REMOVED} interface(s) (veth=${REMOVED}, loop=${LOOP_REMOVED}).${NC}"
else
    echo -e "${YELLOW}No interfaces to remove.${NC}"
fi
echo ""
