FROM ubuntu:24.04

RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      frr \
      frr-pythontools \
      lldpd \
      iproute2 \
      iputils-ping \
      net-tools \
      tcpdump \
      procps \
      ca-certificates \
      bash \
    && rm -rf /var/lib/apt/lists/*

COPY ci-start-frr.sh /usr/local/bin/ci-start-frr.sh
RUN chmod +x /usr/local/bin/ci-start-frr.sh

WORKDIR /root
CMD ["sleep", "infinity"]
