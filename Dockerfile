# Multi-stage build for NetNexus GNS3 Docker image
# 支持开发和生产两种环境：
#   开发环境: docker build --target dev -t netnexus:dev .
#   生产环境: docker build --target production -t netnexus:latest .

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
# Stage 2: 编译阶段（用于生产镜像构建）
# ============================================================
FROM base-dev AS builder

ARG VERSION=dev
ARG GIT_COMMIT=unknown

COPY . /build
WORKDIR /build

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release

RUN chmod +x ./scripts/prod/package.sh && \
    VERSION=${VERSION} ./scripts/prod/package.sh

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
# Stage 4: 生产环境（精简运行时镜像）
# 使用方式: docker build --target production -t netnexus:latest .
# ============================================================
FROM ubuntu:24.04 AS production

LABEL maintainer="NetNexus Team"
LABEL description="NetNexus Network Device for GNS3"
LABEL org.opencontainers.image.title="NetNexus"
LABEL org.opencontainers.image.description="BGP/BMP/RPKI Network Device"
LABEL org.opencontainers.image.vendor="NetNexus Team"
LABEL org.opencontainers.image.source="https://github.com/yourrepo/netnexus"

ARG VERSION=dev
ARG GIT_COMMIT=unknown
LABEL org.opencontainers.image.version="${VERSION}"
LABEL org.opencontainers.image.revision="${GIT_COMMIT}"

# 仅安装运行时依赖，不包含编译工具
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libglib2.0-0 \
    libxml2 \
    libsqlite3-0 \
    iproute2 \
    iputils-ping \
    net-tools \
    tcpdump \
    telnet \
    && rm -rf /var/lib/apt/lists/*

# 从 builder 阶段复制部署包并安装
COPY --from=builder /build/package/netnexus-${VERSION}.tar.gz /tmp/
RUN tar -xzf /tmp/netnexus-${VERSION}.tar.gz -C /tmp && \
    cd /tmp/netnexus-${VERSION} && \
    INSTALL_DIR=/opt/netnexus ./scripts/deploy.sh && \
    rm -rf /tmp/netnexus-*

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
