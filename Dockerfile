# Multi-stage build for NetNexus GNS3 Docker image
# 支持三种构建目标：
#   开发环境:   docker build --target dev -t netnexus:dev .
#   调试镜像:   docker build --target debug --build-arg BUILD_TYPE=Debug -t netnexus:debug .
#   生产镜像:   docker build --target production -t netnexus:latest .

# ============================================================
# Stage 1: 开发基础镜像（包含所有构建和调试工具）
# ============================================================
FROM ubuntu:24.04 AS base-dev

RUN apt-get update && \
    apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libglib2.0-dev \
    libxml2-dev \
    libsqlite3-dev \
    clang-format \
    clang-tidy \
    gdb \
    tcpdump \
    iproute2 \
    iputils-ping \
    net-tools \
    telnet \
    && rm -rf /var/lib/apt/lists/*

# ============================================================
# Stage 2: 编译阶段
# BUILD_TYPE 控制 Debug/Release，默认 Release
# ============================================================
FROM base-dev AS builder

ARG BUILD_TYPE=Release
ARG ENABLE_ASAN=OFF
ARG VERSION=dev
ARG GIT_COMMIT=unknown

COPY . /build
WORKDIR /build

RUN cmake -B build -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DENABLE_ASAN=${ENABLE_ASAN} && \
    cmake --build build --config ${BUILD_TYPE}

# ============================================================
# Stage 3: 开发环境（用于容器内编码、构建、调试）
# 使用方式: docker build --target dev -t netnexus:dev .
# 源码通过 volume 挂载到 /workspace
# ============================================================
FROM base-dev AS dev

LABEL maintainer="NetNexus Team"
LABEL description="NetNexus Development Environment"

WORKDIR /workspace

EXPOSE 3788

# 开发环境不需要 NN_WORK_DIR，源码目录下的 XML 会被自动发现
CMD ["/bin/bash"]

# ============================================================
# Stage 4: 部署基础镜像（debug 和 production 共享）
# 包含运行时依赖和构建产物安装，不含开发工具
# ============================================================
FROM ubuntu:24.04 AS deployable-base

ARG ENABLE_ASAN=OFF
ARG VERSION=dev
ARG GIT_COMMIT=unknown

LABEL maintainer="NetNexus Team"
LABEL org.opencontainers.image.title="NetNexus"
LABEL org.opencontainers.image.description="BGP/BMP/RPKI Network Device"
LABEL org.opencontainers.image.vendor="NetNexus Team"
LABEL org.opencontainers.image.version="${VERSION}"
LABEL org.opencontainers.image.revision="${GIT_COMMIT}"

# 仅安装运行时依赖,不包含编译工具和开发工具
# docker.io-cli: dev swap-image 命令需要,容器只用客户端(通过挂载的
# /var/run/docker.sock 调用宿主 docker daemon)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libxml2 \
    libsqlite3-0 \
    tcpdump \
    iproute2 \
    iputils-ping \
    net-tools \
    telnet \
    docker.io \
    && if [ "${ENABLE_ASAN}" = "ON" ]; then apt-get install -y --no-install-recommends libasan8; fi \
    && rm -rf /var/lib/apt/lists/*

# 从 builder 阶段直接复制构建产物并安装
COPY --from=builder /build/build/bin/       /opt/netnexus/bin/
COPY --from=builder /build/build/lib/       /opt/netnexus/lib/
COPY --from=builder /build/scripts/prod/start.sh      /opt/netnexus/scripts/
COPY --from=builder /build/scripts/prod/supervise.sh  /opt/netnexus/scripts/
COPY --from=builder /build/scripts/prod/gns3-entry.sh /opt/netnexus/scripts/
COPY --from=builder /build/scripts/prod/swap-image.sh /opt/netnexus/scripts/

# 复制各模块资源文件（commands.xml、module.conf 等）
RUN mkdir -p /opt/netnexus/data /opt/netnexus/resources
COPY --from=builder /build/src/ /build/src/
RUN find /build/src -type d -name resources | while read d; do \
        mod=$(basename "$(dirname "$d")"); \
        mkdir -p /opt/netnexus/resources/"$mod"; \
        cp "$d"/* /opt/netnexus/resources/"$mod"/; \
    done && rm -rf /build

RUN chmod +x /opt/netnexus/bin/* /opt/netnexus/scripts/*.sh && \
    echo "${VERSION}" > /opt/netnexus/VERSION

ENV LD_LIBRARY_PATH=/opt/netnexus/lib
ENV NN_WORK_DIR=/opt/netnexus

EXPOSE 3788

WORKDIR /opt/netnexus/bin

# GNS3 兼容性标签
LABEL com.gns3.capabilities="SYS_PTRACE,NET_ADMIN"
LABEL com.gns3.security-opt="seccomp=unconfined"

HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD netstat -tuln | grep 3788 || exit 1

VOLUME ["/opt/netnexus/data"]

CMD ["/opt/netnexus/scripts/gns3-entry.sh"]

# ============================================================
# Stage 5: 调试镜像（含 gdb 和 tcpdump，用于问题排查）
# 使用方式: docker build --target debug --build-arg BUILD_TYPE=Debug -t netnexus:debug .
# ============================================================
FROM deployable-base AS debug

LABEL description="NetNexus Debug Image (with gdb/tcpdump)"

RUN apt-get update && \
    apt-get install -y --no-install-recommends gdb tcpdump && \
    rm -rf /var/lib/apt/lists/*

# ============================================================
# Stage 6: 生产镜像（精简运行时镜像，不含开发工具）
# 使用方式: docker build --target production -t netnexus:latest .
# ============================================================
FROM deployable-base AS production

LABEL description="NetNexus Network Device for GNS3"
