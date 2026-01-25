#!/bin/bash
# Build NetNexus Docker image for GNS3 (Multi-stage build)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "======================================"
echo "Building NetNexus for GNS3"
echo "======================================"
echo ""

# Build Docker image (multi-stage build - no need to pre-build)
echo -e "${YELLOW}Building Docker image with multi-stage build...${NC}"
echo "This will build NetNexus inside the container for compatibility."
echo ""

IMAGE_NAME="netnexus"

# Read version from VERSION file
if [ -f "VERSION" ]; then
    VERSION=$(cat VERSION | tr -d '[:space:]')
else
    VERSION="dev"
fi

# Get git commit hash if available
GIT_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo "unknown")

echo "Version: ${VERSION}"
echo "Git commit: ${GIT_COMMIT}"
echo ""

# Build with multiple tags
docker build \
    --build-arg VERSION=${VERSION} \
    --build-arg GIT_COMMIT=${GIT_COMMIT} \
    -t ${IMAGE_NAME}:latest \
    -t ${IMAGE_NAME}:${VERSION} \
    -t ${IMAGE_NAME}:${VERSION}-${GIT_COMMIT} \
    .

if [ $? -eq 0 ]; then
    echo ""
    echo -e "${GREEN}✓ Docker image built successfully${NC}"
else
    echo ""
    echo -e "${RED}Error: Docker build failed!${NC}"
    exit 1
fi
echo ""

# Display image info
echo "======================================"
echo "Build Complete!"
echo "======================================"
echo ""
echo "Image: ${IMAGE_NAME}"
echo "Tags:"
echo "  - ${IMAGE_NAME}:latest"
echo "  - ${IMAGE_NAME}:${VERSION}"
echo "  - ${IMAGE_NAME}:${VERSION}-${GIT_COMMIT}"
echo "Size: $(docker images ${IMAGE_NAME}:latest --format "{{.Size}}")"
echo ""
echo "Next steps:"
echo "1. Test locally:"
echo "   docker run -it --rm -p 3788:3788 ${IMAGE_NAME}:latest"
echo "   docker run -it --rm -p 3788:3788 ${IMAGE_NAME}:${VERSION}"
echo "   (In another terminal) telnet localhost 3788"
echo "   enter bash (In another terminal) sudo docker exec -it <container_id> /bin/bash"
echo ""
echo "3. Import into GNS3:"
echo "   - Open GNS3"
echo "   - Edit → Preferences → Docker containers → New"
echo "   - Select image: ${IMAGE_NAME}:latest or ${IMAGE_NAME}:${VERSION}"
echo ""
echo "4. Export for distribution:"
echo "   docker save ${IMAGE_NAME}:${VERSION} | gzip > netnexus-${VERSION}.tar.gz"
echo ""
echo "5. Push to registry (if configured):"
echo "   docker push ${IMAGE_NAME}:latest"
echo "   docker push ${IMAGE_NAME}:${VERSION}"
echo ""
