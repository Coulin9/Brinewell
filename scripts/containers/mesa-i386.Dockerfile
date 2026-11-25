FROM debian:trixie
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential gcc-i686-linux-gnu g++-i686-linux-gnu binutils-i686-linux-gnu \
      pkg-config meson ninja-build python3 python3-mako python3-packaging python3-yaml \
      ca-certificates curl xz-utils libxml2-dev libexpat1-dev libffi-dev git \
      libc6-dev:i386 libdrm-dev:i386 libx11-xcb-dev:i386 libxcb1-dev:i386 \
      libxcb-randr0-dev:i386 libxcb-dri3-dev:i386 libxcb-present-dev:i386 \
      libxcb-shm0-dev:i386 libxcb-sync-dev:i386 libxcb-xfixes0-dev:i386 \
      libxshmfence-dev:i386 libwayland-dev:i386 libexpat1-dev:i386 \
      zlib1g-dev:i386 libzstd-dev:i386 libffi-dev:i386 libstdc++-14-dev:i386 \
 && rm -rf /var/lib/apt/lists/*

# Only the native scanner is upgraded; the guest uses trixie's i386 Wayland ABI.
RUN curl -fsSL -o /tmp/wayland.tar.xz https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.25.0/downloads/wayland-1.25.0.tar.xz \
 && echo "c065f040afdff3177680600f249727e41a1afc22fccf27222f15f5306faa1f03  /tmp/wayland.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/wayland.tar.xz -C /tmp \
 && meson setup /tmp/wayland-1.25.0/build /tmp/wayland-1.25.0 --prefix=/usr/local -Dlibraries=false -Dscanner=true -Ddocumentation=false -Dtests=false \
 && meson install -C /tmp/wayland-1.25.0/build \
 && rm -rf /tmp/wayland-1.25.0 /tmp/wayland.tar.xz

RUN curl -fsSL -o /tmp/protocols.tar.xz https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.49/downloads/wayland-protocols-1.49.tar.xz \
 && echo "ec4c8f74942d6dff7ace8b4ce4764f0ef9ff618a935d974ea77edee2ad240b14  /tmp/protocols.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/protocols.tar.xz -C /tmp \
 && meson setup /tmp/wayland-protocols-1.49/build /tmp/wayland-protocols-1.49 --prefix=/usr -Dtests=false \
 && meson install -C /tmp/wayland-protocols-1.49/build \
 && rm -rf /tmp/wayland-protocols-1.49 /tmp/protocols.tar.xz
