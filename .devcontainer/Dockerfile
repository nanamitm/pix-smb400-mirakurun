# PIX-SMB400 BS4K — release build environment
FROM mcr.microsoft.com/devcontainers/base:trixie

# Build & deploy dependencies:
#   build-essential        : make + native gcc (Makefile targets, npm native modules)
#   gcc-arm-linux-gnueabi  : ARM32 cross-compiler for bin/ (tuner-stream-bs-ng, b61dec)
#   libssl-dev             : OpenSSL headers for b61dec
#   binwalk                : extract the initramfs cpio from kernel.img
#   cpio                   : unpack the firmware initramfs (build_initramfs.sh, host side)
#   adb                    : pull android-libs / push binaries & scripts to the device
#   python3-pycryptodome   : make_usb_boot.py (bootargs.bin / RSA key generation)
#   nodejs, npm            : build Mirakurun-BS4K (npm install && npm run build)
#   curl                   : make start / make test, setup_proot.sh downloads
#   git                    : clone Mirakurun-BS4K
#
# mkimage/u-boot-tools are intentionally omitted: build_initramfs.sh builds the
# uImage inside a throwaway Docker container (docker-in-docker feature).
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        gcc-arm-linux-gnueabi \
        libssl-dev \
        binwalk \
        cpio \
        adb \
        python3-pycryptodome \
        nodejs \
        npm \
        curl \
        git \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*
