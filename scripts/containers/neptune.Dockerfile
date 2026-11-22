FROM debian:trixie
RUN dpkg --add-architecture amd64 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu libc6-dev-amd64-cross \
      bison flex ca-certificates file meson ninja-build pkg-config \
      python3 python3-mako python3-packaging python3-yaml \
      libdrm-dev libxcb1-dev libxcb-dri3-dev libxcb-present-dev \
      libdrm-dev:amd64 libxcb1-dev:amd64 libxcb-dri3-dev:amd64 libxcb-present-dev:amd64 \
      xz-utils \
 && rm -rf /var/lib/apt/lists/*
