#!/bin/bash
# setup_proot.sh — one-time setup of Alpine Linux ARM + Node.js on SMB400.
#
# Run from the development host (requires ADB connection to device):
#   make setup-runtime ADB_TARGET=<device-ip>:5555
#
# What this does:
#   1. Downloads the Alpine Linux ARM minimal rootfs
#   2. Pushes it to /data/local/tmp/ on the device and extracts it
#      (skipped when the rootfs is already provisioned)
#   2.5. Verifies (and repairs) bin/busybox + bin/sh, normalizes bin/sh to a
#        relative 'busybox' link, then smoke-tests /bin/sh
#   3. Configures DNS
#   4. chroots into the rootfs (adb is root) and installs Node.js + npm via apk
#
# Safe to re-run, also while Mirakurun is running.
# The runtime (start_mirakurun.sh) also uses chroot, so no proot is needed.

set -euo pipefail

ADB_TARGET="${1:-}"
if command -v adb.exe >/dev/null 2>&1; then
    ADB_BIN="adb.exe"
else
    ADB_BIN="adb"
fi
if [ -n "$ADB_TARGET" ]; then
    ADB="$ADB_BIN -s $ADB_TARGET"
else
    ADB="$ADB_BIN"
fi

DEVICE_TMP=/data/local/tmp
ROOTFS_DIR="$DEVICE_TMP/mirakurun-root"
WORK_DIR=$(mktemp -d)
trap "rm -rf '$WORK_DIR'" EXIT

# Alpine 3.23 系は nodejs 24.x (LTS) を提供する。
ALPINE_VERSION=3.23
ALPINE_PATCH=3.23.6
ALPINE_ARCH=armhf
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/${ALPINE_ARCH}/alpine-minirootfs-${ALPINE_PATCH}-${ALPINE_ARCH}.tar.gz"

# Download the minirootfs once per run (Step 2 extracts it; Step 2.5 reuses it
# for repairs). Android's toybox tar cannot exec gunzip, so decompress on the
# host and push an uncompressed .tar (extracted with `tar xf` on the device).
ensure_alpine_tar() {
    if [ ! -f "$WORK_DIR/alpine-rootfs.tar" ]; then
        curl -L -o "$WORK_DIR/alpine-rootfs.tar.gz" "$ALPINE_URL"
        gunzip "$WORK_DIR/alpine-rootfs.tar.gz"   # → $WORK_DIR/alpine-rootfs.tar
    fi
}

# Idempotent: skip the download/extract when the rootfs is already provisioned.
# Re-extracting would also fail: start_mirakurun.sh replaces Alpine's /var/run
# symlink with a real directory, which tar cannot turn back into a symlink.
# tr -d '\r\n' strips the CRLF that `adb shell` appends to captured output.
alpine_present=$($ADB shell "[ -f '$ROOTFS_DIR/etc/alpine-release' ] && echo yes || echo no" | tr -d '\r\n')
case "$alpine_present" in
    *yes*)
        echo "=== Step 1-2: Alpine rootfs already on device — skipping download/extract. ==="
        ;;
    *)
        echo "=== Step 1: Download Alpine ${ALPINE_VERSION} (${ALPINE_ARCH}) ==="
        ensure_alpine_tar

        echo "=== Step 2: Push and extract Alpine rootfs ==="
        $ADB shell mkdir -p "$ROOTFS_DIR"
        $ADB push "$WORK_DIR/alpine-rootfs.tar" "$DEVICE_TMP/alpine-rootfs.tar"
        $ADB shell "cd '$ROOTFS_DIR' && tar xf '$DEVICE_TMP/alpine-rootfs.tar'"
        $ADB shell "rm '$DEVICE_TMP/alpine-rootfs.tar'"
        ;;
esac

echo "=== Step 2.5: Verify busybox and /bin/sh ==="
# Alpine ships bin/sh as an absolute symlink -> /bin/busybox. Outside the chroot
# (Android namespace) /bin/busybox does not exist, so toybox `ls` and `test -e`
# report the link as missing even though it resolves fine inside the chroot.
# Normalize it to the relative link `busybox` so it resolves in both.
# Also repair a busybox left missing or wrong-architecture by an interrupted
# extraction, which would make every chroot fail with "No such file or
# directory". Runs even when Step 1-2 was skipped, so broken installs self-heal.
sh_link=$($ADB shell "readlink '$ROOTFS_DIR/bin/sh' 2>/dev/null" 2>/dev/null | tr -d '\r\n') || sh_link=""
# ELF header of bin/busybox: magic + 32-bit + little-endian (first 12 hex chars),
# e_machine = ARM 0x0028 (last 4 hex chars).
bb_header=$($ADB shell "od -An -tx1 -N20 '$ROOTFS_DIR/bin/busybox' 2>/dev/null" 2>/dev/null | tr -d ' \r\n') || bb_header=""
if [ "${bb_header:0:12}" != "7f454c460101" ] || [ "${bb_header: -4}" != "2800" ]; then
    echo "[!] bin/busybox is missing or not an armhf ELF32 ARM binary — restoring from minirootfs..."
    ensure_alpine_tar
    mkdir -p "$WORK_DIR/alpine-fix"
    tar -C "$WORK_DIR/alpine-fix" -xf "$WORK_DIR/alpine-rootfs.tar" ./bin/busybox
    $ADB push "$WORK_DIR/alpine-fix/bin/busybox" "$ROOTFS_DIR/bin/busybox"
    $ADB shell chmod 755 "$ROOTFS_DIR/bin/busybox"
fi
# Remove-then-create instead of `ln -sf`: toybox ln has no reliable -f behavior.
if [ "$sh_link" = "busybox" ]; then
    echo "[=] bin/sh -> busybox (relative) — OK."
else
    echo "[*] Relinking bin/sh -> busybox (was: ${sh_link:-missing})"
    $ADB shell "rm -f '$ROOTFS_DIR/bin/sh'; ln -s busybox '$ROOTFS_DIR/bin/sh'"
fi
sh_resolves=$($ADB shell "test -e '$ROOTFS_DIR/bin/sh' && echo yes || echo no" | tr -d '\r\n')
if [ "$sh_resolves" != "yes" ]; then
    echo "[!] $ROOTFS_DIR/bin/sh does not resolve from the Android namespace."
    exit 1
fi
# Functional smoke test: catches an incomplete rootfs (e.g. a missing
# /lib/ld-musl-armhf.so.1) that the file-level checks above cannot see.
sh_probe=$($ADB shell "chroot '$ROOTFS_DIR' /bin/sh -c 'echo sh-ok' 2>&1" || true)
case "$sh_probe" in
    *sh-ok*)
        echo "[=] chroot /bin/sh OK."
        ;;
    *)
        echo "[!] chroot /bin/sh failed: $(echo "$sh_probe" | tr -d '\r\n')"
        echo "[!] The Alpine rootfs is incomplete (e.g. an interrupted extraction)."
        echo "[!] Recreate it: $ADB shell rm -rf '$ROOTFS_DIR' && make setup-runtime"
        exit 1
        ;;
esac

echo "=== Step 3: Configure Alpine DNS ==="
$ADB shell "echo 'nameserver 8.8.8.8' > '$ROOTFS_DIR/etc/resolv.conf'"

echo "=== Step 4: Install Node.js + npm inside chroot ==="
# adb runs as root, so we chroot directly (same mechanism as the runtime).
# proc + /dev are needed for apk (TLS uses /dev/urandom). A running Mirakurun
# session has already mounted both (start_mirakurun.sh), so mount only what is
# missing and unmount only what we mounted — unmounting the session's /proc or
# /dev would break it.
$ADB shell '
ROOTFS=/data/local/tmp/mirakurun-root
mounted_proc=0; mounted_dev=0
if ! grep -q " $ROOTFS/proc " /proc/mounts; then
    mount -t proc proc "$ROOTFS/proc" && mounted_proc=1
fi
if ! grep -q " $ROOTFS/dev " /proc/mounts; then
    mount -o bind /dev "$ROOTFS/dev" && mounted_dev=1
fi
chroot "$ROOTFS" /bin/sh -c "export PATH=/usr/sbin:/usr/bin:/sbin:/bin; apk update && apk add nodejs npm"
RC=$?
[ "$mounted_dev" = 1 ]  && umount "$ROOTFS/dev"
[ "$mounted_proc" = 1 ] && umount "$ROOTFS/proc"
exit $RC
'

echo ""
echo "=== Setup complete ==="
echo "Node.js version:"
$ADB shell "chroot '$ROOTFS_DIR' /bin/sh -c 'export PATH=/usr/sbin:/usr/bin:/sbin:/bin; node --version'"
