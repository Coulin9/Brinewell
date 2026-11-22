FROM debian:trixie
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      bc bison build-essential ca-certificates cpio dwarves file flex git kmod \
      libelf-dev libssl-dev make patch python3 rsync xz-utils zstd \
 && rm -rf /var/lib/apt/lists/*
