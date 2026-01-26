# Multi-stage build for NetNexus GNS3 Docker image
# This ensures binary compatibility by building inside the container

# Stage 1: Build environment
FROM ubuntu:24.04 AS builder

# Install build dependencies
RUN apt-get update && \
    apt-get install -y \
    build-essential \
    cmake \
    libglib2.0-dev \
    libxml2-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . /build

# Build NetNexus
WORKDIR /build
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --config Release

# Stage 2: Runtime environment
FROM ubuntu:24.04

# Metadata
LABEL maintainer="NetNexus Team"
LABEL description="NetNexus Network Device for GNS3"
LABEL org.opencontainers.image.title="NetNexus"
LABEL org.opencontainers.image.description="BGP/BMP/RPKI Network Device"
LABEL org.opencontainers.image.vendor="NetNexus Team"
LABEL org.opencontainers.image.source="https://github.com/yourrepo/netnexus"

# Version will be set via build args
ARG VERSION=dev
ARG GIT_COMMIT=unknown
LABEL org.opencontainers.image.version="${VERSION}"
LABEL org.opencontainers.image.revision="${GIT_COMMIT}"

# Install runtime dependencies and debugging tools
RUN apt-get update && \
    apt-get install -y \
    libglib2.0-0 \
    libxml2 \
    iproute2 \
    iputils-ping \
    net-tools \
    gdb \
    tcpdump \
    && rm -rf /var/lib/apt/lists/*

# Create application directory
RUN mkdir -p /opt/netnexus/bin /opt/netnexus/config

# Copy built binaries from builder stage
COPY --from=builder /build/build/bin/netnexus /opt/netnexus/bin/
COPY --from=builder /build/build/lib/*.so* /opt/netnexus/bin/

# Copy XML configuration files
COPY --from=builder /build/src/bgp/commands.xml /opt/netnexus/config/bgp_commands.xml
COPY --from=builder /build/src/dev/commands.xml /opt/netnexus/config/dev_commands.xml
COPY --from=builder /build/src/cfg/commands.xml /opt/netnexus/config/cfg_commands.xml

# Set library path and XML directory
ENV LD_LIBRARY_PATH=/opt/netnexus/bin
ENV NN_XML_DIR=/opt/netnexus/config

# Expose telnet console port
EXPOSE 3788

# Set working directory
WORKDIR /opt/netnexus/bin

# GNS3 compatibility: Request debugging capabilities
# These labels help GNS3 understand the container needs debugging support
LABEL com.gns3.capabilities="SYS_PTRACE,NET_ADMIN"
LABEL com.gns3.security-opt="seccomp=unconfined"

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD netstat -tuln | grep 3788 || exit 1

# Run NetNexus
CMD ["/opt/netnexus/bin/netnexus"]
