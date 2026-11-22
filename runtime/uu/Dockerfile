FROM debian:trixie
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates clang cmake g++ git lld ninja-build pkg-config python3 python3-packaging \
      curl iptables iproute2 nftables zlib1g-dev libssl-dev \
 && rm -rf /var/lib/apt/lists/*
