#!/bin/bash
# Show status of development network interfaces

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo "======================================"
echo "NetNexus Development Interfaces"
echo "======================================"
echo ""

# Check for veth interfaces
VETH_COUNT=$(ip link show | grep -c "veth" || echo "0")

if [ "$VETH_COUNT" -eq 0 ]; then
    echo -e "${YELLOW}No virtual interfaces found.${NC}"
    echo ""
    echo "To create interfaces: sudo ./scripts/setup-dev-interfaces.sh"
    exit 0
fi

echo -e "${BLUE}Network Interfaces:${NC}"
ip link show | grep veth | while read line; do
    IFACE=$(echo $line | awk '{print $2}' | sed 's/:$//' | sed 's/@.*//')
    STATE=$(echo $line | grep -o "state [A-Z]*" | awk '{print $2}')
    
    if [ "$STATE" = "UP" ]; then
        echo -e "  ${GREEN}✓${NC} $IFACE (${STATE})"
    else
        echo -e "  ${YELLOW}⚠${NC} $IFACE (${STATE})"
    fi
done

echo ""
echo -e "${BLUE}IP Addresses:${NC}"
ip addr show | grep "inet.*veth" | while read line; do
    IP=$(echo $line | awk '{print $2}')
    IFACE=$(echo $line | awk '{print $NF}')
    echo "  $IFACE: $IP"
done

echo ""
echo -e "${BLUE}Statistics:${NC}"
for i in {0..3}; do
    if ip link show veth$i &> /dev/null; then
        RX=$(ip -s link show veth$i | grep "RX:" -A 1 | tail -1 | awk '{print $1}')
        TX=$(ip -s link show veth$i | grep "TX:" -A 1 | tail -1 | awk '{print $1}')
        echo "  veth$i: RX ${RX} bytes, TX ${TX} bytes"
    fi
done

echo ""
echo "Commands:"
echo "  Setup:   sudo ./scripts/setup-dev-interfaces.sh"
echo "  Cleanup: sudo ./scripts/cleanup-dev-interfaces.sh"
echo "  Test:    ping -I veth0 192.168.10.2"
echo ""
