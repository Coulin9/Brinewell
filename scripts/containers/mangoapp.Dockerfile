FROM localhost/brinewell-guest-builder
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libdbus-1-dev libglfw3-dev libxkbcommon-dev \
 && rm -rf /var/lib/apt/lists/*
