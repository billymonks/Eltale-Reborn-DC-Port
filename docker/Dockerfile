# Dreamcast toolchain for Quest 64 DC: KallistiOS v2.2.1 with the GCC 14.3.0
# dc-chain profile (m4-single ABI), plus the host tools the build needs
# (cmake/ninja for N64Recomp, mkdcdisc for CDI images).
#
#   docker build -t q64dc-toolchain docker
#
# Building the toolchain takes a while (30-90 minutes depending on the machine).

FROM ubuntu:24.04

ARG KOS_VERSION=v2.2.1
ARG DC_CHAIN_PROFILE=14.3.0
ARG MAKEJOBS=8

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc g++ make cmake ninja-build meson pkg-config \
        git curl wget ca-certificates patch bzip2 xz-utils tar gawk \
        texinfo bison flex python3 python3-pip \
        libgmp-dev libmpfr-dev libmpc-dev libelf-dev \
        libjpeg-dev libpng-dev libisofs-dev \
    && rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/toolchains/dc \
    && git clone --depth 1 --branch ${KOS_VERSION} https://github.com/KallistiOS/KallistiOS.git /opt/toolchains/dc/kos

RUN cd /opt/toolchains/dc/kos/utils/dc-chain \
    && make build toolchain_profile=${DC_CHAIN_PROFILE} makejobs=${MAKEJOBS} \
    && make clean || true

RUN cp /opt/toolchains/dc/kos/doc/environ.sh.sample /opt/toolchains/dc/kos/environ.sh

RUN bash -c "source /opt/toolchains/dc/kos/environ.sh \
    && cd /opt/toolchains/dc/kos && make -j${MAKEJOBS}"

RUN git clone --depth 1 https://gitlab.com/simulant/mkdcdisc.git /tmp/mkdcdisc \
    && cd /tmp/mkdcdisc && meson setup builddir && meson compile -C builddir \
    && cp builddir/mkdcdisc /usr/local/bin/ \
    && rm -rf /tmp/mkdcdisc

RUN echo 'source /opt/toolchains/dc/kos/environ.sh' >> /etc/bash.bashrc

WORKDIR /src
