#!/bin/bash
# Run NetNexus Docker container in debug mode with gdb support

set -e

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

IMAGE_NAME="netnexus"
IMAGE_TAG="latest"
CONTAINER_NAME="netnexus-debug"

echo "======================================"
echo "NetNexus Debug Mode"
echo "======================================"
echo ""

# Check if container already exists
if sudo docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo -e "${YELLOW}Container ${CONTAINER_NAME} already exists. Removing...${NC}"
    sudo docker stop ${CONTAINER_NAME} 2>/dev/null || true
    sudo docker rm ${CONTAINER_NAME} 2>/dev/null || true
fi

echo -e "${BLUE}Starting NetNexus in debug mode...${NC}"
echo ""
echo "Capabilities enabled:"
echo "  - SYS_PTRACE (for gdb)"
echo "  - NET_ADMIN (for network debugging)"
echo ""

# Start container with debug capabilities
sudo docker run -d \
    --name ${CONTAINER_NAME} \
    --cap-add=SYS_PTRACE \
    --cap-add=NET_ADMIN \
    --security-opt seccomp=unconfined \
    -p 3788:3788 \
    ${IMAGE_NAME}:${IMAGE_TAG}

# Wait for container to be healthy
echo -e "${BLUE}Waiting for container to be ready...${NC}"
sleep 2

# Check if container is running
if sudo docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo -e "${GREEN}✓ Container started successfully${NC}"
    echo ""
    echo "======================================"
    echo "Debug Commands"
    echo "======================================"
    echo ""
    echo "1. Enter container:"
    echo -e "   ${BLUE}sudo docker exec -it ${CONTAINER_NAME} /bin/bash${NC}"
    echo ""
    echo "2. Attach gdb to NetNexus process:"
    echo -e "   ${BLUE}sudo docker exec -it ${CONTAINER_NAME} gdb -p 1${NC}"
    echo ""
    echo "3. Run tcpdump:"
    echo -e "   ${BLUE}sudo docker exec -it ${CONTAINER_NAME} tcpdump -i any${NC}"
    echo ""
    echo "4. View logs:"
    echo -e "   ${BLUE}sudo docker logs -f ${CONTAINER_NAME}${NC}"
    echo ""
    echo "5. Connect via telnet:"
    echo -e "   ${BLUE}telnet localhost 3788${NC}"
    echo ""
    echo "6. Stop container:"
    echo -e "   ${BLUE}sudo docker stop ${CONTAINER_NAME}${NC}"
    echo ""
else
    echo -e "${YELLOW}⚠ Container failed to start${NC}"
    echo "Check logs with: sudo docker logs ${CONTAINER_NAME}"
    exit 1
fi
