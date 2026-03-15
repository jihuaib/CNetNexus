#!/bin/bash
# Build NetNexus Docker image (multi-stage build)

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

usage() {
    cat <<'EOF'
Usage: scripts/dev/build-docker-image.sh [options]

Options:
  --docker-image <name[:tag]>  Add one extra docker tag for this build.
                               Examples:
                                 --docker-image netnexus-ci:localtest
                                 --docker-image myrepo/netnexus
  -h, --help                   Show this help.

Notes:
  - Default output tags are still generated:
      netnexus:latest
      netnexus:<VERSION>
      netnexus:<VERSION>-<GIT_COMMIT>
  - PLATFORM env is supported for cross-build:
      PLATFORM=linux/amd64 scripts/dev/build-docker-image.sh
EOF
}

EXTRA_DOCKER_IMAGE=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --docker-image)
            if [[ -z "${2:-}" ]]; then
                echo "Error: --docker-image requires a value" >&2
                usage
                exit 1
            fi
            EXTRA_DOCKER_IMAGE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Error: unknown argument '$1'" >&2
            usage
            exit 1
            ;;
    esac
done

if [[ -n "${EXTRA_DOCKER_IMAGE}" && "${EXTRA_DOCKER_IMAGE}" != *:* ]]; then
    EXTRA_DOCKER_IMAGE="${EXTRA_DOCKER_IMAGE}:latest"
fi

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "======================================"
echo "Building NetNexus Docker image"
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

# 支持跨平台构建：默认当前平台，可通过 PLATFORM 环境变量指定
# 例如 ARM Mac 上构建 x86 镜像：PLATFORM=linux/amd64 ./scripts/dev/build-docker-image.sh
PLATFORM="${PLATFORM:-}"
PLATFORM_FLAG=()
if [ -n "${PLATFORM}" ]; then
    PLATFORM_FLAG=(--platform "${PLATFORM}")
    echo "Cross-build platform: ${PLATFORM}"
    echo ""
fi

BUILD_TAGS=(
    -t "${IMAGE_NAME}:latest"
    -t "${IMAGE_NAME}:${VERSION}"
    -t "${IMAGE_NAME}:${VERSION}-${GIT_COMMIT}"
)
if [ -n "${EXTRA_DOCKER_IMAGE}" ]; then
    BUILD_TAGS+=(-t "${EXTRA_DOCKER_IMAGE}")
    echo "Extra tag: ${EXTRA_DOCKER_IMAGE}"
    echo ""
fi

# Build with multiple tags
# --network=host 让构建容器复用宿主机网络，解决容器内 DNS 解析失败的问题
docker build \
    --network=host \
    "${PLATFORM_FLAG[@]}" \
    --target production \
    --build-arg VERSION=${VERSION} \
    --build-arg GIT_COMMIT=${GIT_COMMIT} \
    "${BUILD_TAGS[@]}" \
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
if [ -n "${EXTRA_DOCKER_IMAGE}" ]; then
    echo "  - ${EXTRA_DOCKER_IMAGE}"
fi
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
