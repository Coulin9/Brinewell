FROM debian:trixie
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf automake bc bison build-essential ca-certificates cpio curl \
      dpkg-dev dosfstools e2fsprogs file flex gcc g++ gdisk gzip git kmod \
      libblkid-dev libelf-dev libexpat1-dev libglvnd-dev liblzo2-dev libssl-dev libtool \
      libzstd-dev libdrm-dev libx11-dev libxext-dev libxcb1-dev libxcb-randr0-dev libxcb-shm0-dev \
      libxcb-present-dev libxcb-sync-dev libxcb-dri3-dev libxcb-xfixes0-dev \
      libxcb-glx0-dev libx11-xcb-dev libxxf86vm-dev libxcb-keysyms1-dev libxrandr-dev libxi-dev libxt-dev libxmu-dev libxpm-dev libxshmfence-dev libwayland-dev libwayland-egl-backend-dev wayland-protocols libudev-dev \
      libvulkan-dev glslang-tools spirv-tools liblzma-dev mtools \
      make meson ninja-build patch pkg-config python3 python3-mako \
      python3-packaging python3-yaml rsync uuid-dev xz-utils zlib1g-dev zstd \
 && rm -rf /var/lib/apt/lists/*

# Mesa 26.3 requires wayland-protocols 1.49, whose staging interfaces carry the
# `frozen` attribute, and its meson wayland module insists the scanner match the
# wayland-client version exactly.  Debian trixie's 1.23.1 satisfies neither, so
# install wayland 1.25.0 (scanner, libraries and pkg-config files) under
# /usr/local, which takes precedence over the distro copy.
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends libxml2-dev \
 && curl -fsSL -o /tmp/wayland.tar.xz https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.25.0/downloads/wayland-1.25.0.tar.xz \
 && echo "c065f040afdff3177680600f249727e41a1afc22fccf27222f15f5306faa1f03  /tmp/wayland.tar.xz" | sha256sum -c - \
 && tar -xJf /tmp/wayland.tar.xz -C /tmp \
 && meson setup /tmp/wayland-1.25.0/build /tmp/wayland-1.25.0 --prefix=/usr/local -Dtests=false -Ddocumentation=false \
 && ninja -C /tmp/wayland-1.25.0/build \
 && meson install -C /tmp/wayland-1.25.0/build \
 && rm -rf /tmp/wayland-1.25.0 /tmp/wayland.tar.xz /var/lib/apt/lists/*
