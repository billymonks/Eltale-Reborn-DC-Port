# q64dc-toolchain plus the compiler for the AICA's ARM7DI: dc-chain's ARM binutils and GCC 8.5.0,
# the last GCC that targets that core. Only needed to rebuild the port's sound firmware
# (runtime/aica/); the build uses the committed binary without it.
#
#   docker build -t q64dc-toolchain-arm docker/arm
#
# The toolchain lands in /opt/toolchains/dc/arm-eabi, where KallistiOS's environ.sh already
# points DC_ARM_CC and friends.

FROM q64dc-toolchain

ARG DC_CHAIN_PROFILE=14.3.0
ARG MAKEJOBS=8

RUN cd /opt/toolchains/dc/kos/utils/dc-chain \
    && make build-arm toolchain_profile=${DC_CHAIN_PROFILE} makejobs=${MAKEJOBS} \
    && (make clean || true) \
    && /opt/toolchains/dc/arm-eabi/bin/arm-eabi-gcc --version
