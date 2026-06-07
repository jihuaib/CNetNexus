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

# core dump 准备：要求宿主机已执行 setup-coredump.sh，core_pattern 指向
# /var/lib/netnexus-cores/。容器把同路径挂进去，崩溃时 core 落到宿主机此目录。
CORES_DIR="/var/lib/netnexus-cores"
sudo mkdir -p "${CORES_DIR}"
sudo chmod 1777 "${CORES_DIR}"

# Start container with debug capabilities
# --ulimit core=-1：突破 docker daemon 默认的 RLIMIT_CORE 硬上限 0
# -v ${CORES_DIR}: 容器内崩溃，core 通过宿主机的 core_pattern 落到此挂载目录
sudo docker run -d \
    --name ${CONTAINER_NAME} \
    --cap-add=SYS_PTRACE \
    --cap-add=NET_ADMIN \
    --security-opt seccomp=unconfined \
    --ulimit core=-1 \
    -v "${CORES_DIR}:${CORES_DIR}" \
    -v /var/run/docker.sock:/var/run/docker.sock \
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
    echo "5. Connect via console:"
    echo -e "   ${BLUE}sudo docker exec -it -e NN_CONSOLE_SOCK=/opt/netnexus/run/console.sock ${CONTAINER_NAME} /opt/netnexus/bin/netnexus-console${NC}"
    echo ""
    echo "6. Stop container:"
    echo -e "   ${BLUE}sudo docker stop ${CONTAINER_NAME}${NC}"
    echo ""
else
    echo -e "${YELLOW}⚠ Container failed to start${NC}"
    echo "Check logs with: sudo docker logs ${CONTAINER_NAME}"
    exit 1
fi
