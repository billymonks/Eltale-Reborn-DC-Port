# Headless Flycast for automated boot tests: runs the disc image under Xvfb and prints the
# Dreamcast's serial console (KOS printf output) to stdout.
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl xvfb xauth imagemagick \
        libgl1 libegl1 libglx-mesa0 libgl1-mesa-dri libvulkan1 \
        libx11-6 libxext6 libxrandr2 libxi6 libxcursor1 libxinerama1 libxxf86vm1 \
        libasound2t64 libpulse0 libudev1 libcurl4t64 \
    && rm -rf /var/lib/apt/lists/*

ARG FLYCAST_VERSION=2.7
RUN curl -fsSL -o /tmp/flycast.AppImage \
        https://github.com/flyinghead/flycast/releases/download/v${FLYCAST_VERSION}/flycast-x86_64-${FLYCAST_VERSION}.AppImage \
    && chmod +x /tmp/flycast.AppImage \
    && cd /opt && /tmp/flycast.AppImage --appimage-extract > /dev/null \
    && mv squashfs-root flycast && rm /tmp/flycast.AppImage
