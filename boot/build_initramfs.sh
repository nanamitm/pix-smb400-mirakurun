#!/usr/bin/env bash
# build_initramfs.sh — PIX-SMB400 initramfs_patched.uimg のビルドスクリプト
#
# 使い方:
#   bash boot/build_initramfs.sh <firmware_cpio>
#
#   <firmware_cpio>: kernel.img を binwalk で展開して取り出した initramfs cpio ファイル
#                   例: _kernel.img.extracted/988000
#
# 必要なもの: mkimage (u-boot-tools) または docker, python3
#   mkimage が PATH に無い場合は docker を使う。MKIMAGE 環境変数で明示指定も可能:
#     MKIMAGE=/path/to/mkimage bash boot/build_initramfs.sh <firmware_cpio>
# 出力: boot/initramfs_patched.uimg（上書き）
#
# 詳細は BOOT.md を参照。

set -euo pipefail

CPIO_SRC="${1:-}"
if [ -z "$CPIO_SRC" ]; then
    echo "Usage: bash $(basename "$0") <path/to/cpio_file>"
    echo "  例: bash boot/build_initramfs.sh /path/to/_kernel.img.extracted/988000"
    exit 1
fi

if [ ! -f "$CPIO_SRC" ]; then
    echo "[!] ファイルが見つかりません: $CPIO_SRC"
    echo "    kernel.img を binwalk で展開し、取り出した cpio ファイルを指定してください。"
    exit 1
fi

# 後続で作業ディレクトリへ cd するため、相対パスをここで絶対パスに解決しておく。
CPIO_ABS="$(realpath "$CPIO_SRC")"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OVERLAY_DIR="$SCRIPT_DIR/initramfs_overlay"
PATCH_SCRIPT="$SCRIPT_DIR/patch_init.py"
WORK_DIR="${WORK_DIR:-/tmp/smb400_initramfs_work}"
OUT="$SCRIPT_DIR/initramfs_patched.uimg"

# --- 0. スクリプト同期チェック (再発防止) ---
# scripts/*.sh が正本。boot/initramfs_overlay/ 側の同名ファイルは内容を同一に
# 保つこと（ファイル名の -/_ 違いのみ許容）。不一致なら中断する。
#
# 対象: smb400-tuner.sh, crash_guard.sh, stop_android_tv.sh
#   scripts/smb400-tuner.sh    ↔ boot/initramfs_overlay/smb400_tuner.sh
#   scripts/crash_guard.sh     ↔ boot/initramfs_overlay/crash_guard.sh
#   scripts/stop_android_tv.sh ↔ boot/initramfs_overlay/stop_android_tv.sh
#
# 対象外: boot/initramfs_overlay/start_proxy.sh は init 起動用のラッパーで、
#   scripts/ 側に同名の正本は無い。
SYNC_FAILED=0
check_sync() {
    # $1: scripts側, $2: overlay側
    if [ ! -f "$1" ] || [ ! -f "$2" ]; then
        return 0
    fi
    if ! diff -q "$1" "$2" >/dev/null; then
        echo "[!] 不一致: $1 と $2 が異なります"
        echo "    先に同期してください: cp $1 $2"
        diff -u "$1" "$2" | head -n 50 || true
        SYNC_FAILED=1
    fi
}
check_sync "$SCRIPT_DIR/../scripts/smb400-tuner.sh"    "$OVERLAY_DIR/smb400_tuner.sh"
check_sync "$SCRIPT_DIR/../scripts/crash_guard.sh"     "$OVERLAY_DIR/crash_guard.sh"
check_sync "$SCRIPT_DIR/../scripts/stop_android_tv.sh" "$OVERLAY_DIR/stop_android_tv.sh"
if [ "$SYNC_FAILED" -ne 0 ]; then
    exit 1
fi
echo "[+] スクリプト同期OK (tuner / crash_guard / stop_android_tv)"

echo "[*] Work dir: $WORK_DIR"
echo "[*] CPIO src: $CPIO_SRC"
echo "[*] Overlay:  $OVERLAY_DIR"
echo "[*] Output:   $OUT"
echo ""

# --- 1. initramfs を展開 ---
rm -rf "$WORK_DIR" && mkdir -p "$WORK_DIR"
echo "[*] initramfs を展開中..."
(cd "$WORK_DIR" && cpio -idm --no-absolute-filenames -F "$CPIO_ABS" 2>/dev/null)
echo "[+] 展開完了"

# --- 2. init バイナリにパッチ ---
echo "[*] init バイナリにパッチを適用中..."
python3 "$PATCH_SCRIPT" "$WORK_DIR/init"

# --- 3. オーバーレイを適用 ---
echo "[*] オーバーレイを適用中..."

# default.prop: 元の initramfs ではシンボリックリンク → 実ファイルに置き換え
rm -f "$WORK_DIR/default.prop"
cp "$OVERLAY_DIR/default.prop"          "$WORK_DIR/default.prop"
cp "$OVERLAY_DIR/dhclient.conf"         "$WORK_DIR/dhclient.conf"
cp "$OVERLAY_DIR/init.pixboot.rc"       "$WORK_DIR/init.pixboot.rc"
cp "$OVERLAY_DIR/init_pix_netdbg.sh"    "$WORK_DIR/init_pix_netdbg.sh"
mkdir -p "$WORK_DIR/initrc"
cp "$OVERLAY_DIR/initrc/logd.rc"        "$WORK_DIR/initrc/logd.rc"

# デバイス起動時に /data/local/tmp/ に自動デプロイされるスクリプト群
cp "$OVERLAY_DIR/start_proxy.sh"     "$WORK_DIR/start_proxy.sh"
cp "$OVERLAY_DIR/crash_guard.sh"     "$WORK_DIR/crash_guard.sh"
cp "$OVERLAY_DIR/stop_android_tv.sh" "$WORK_DIR/stop_android_tv.sh"
cp "$OVERLAY_DIR/smb400_tuner.sh"    "$WORK_DIR/smb400_tuner.sh"
cp "$SCRIPT_DIR/../scripts/mdns_responder.js" "$WORK_DIR/mdns_responder.js"

# init.rc への import 挿入（冪等）
if ! grep -q 'import /init.pixboot.rc' "$WORK_DIR/init.rc"; then
    sed -i '0,/^import \/init\.usb\.rc$/s||import /init.usb.rc\nimport /init.pixboot.rc|' \
        "$WORK_DIR/init.rc"
    echo "[+] init.rc: import /init.pixboot.rc を挿入"
else
    echo "[=] init.rc: import /init.pixboot.rc は既に存在"
fi

echo "[+] オーバーレイ適用完了"

# --- 4. パーミッション設定 ---
find "$WORK_DIR" -name "*.rc"  | xargs chmod 644
find "$WORK_DIR" -name "*.sh"  | xargs chmod 755
chmod 755 "$WORK_DIR/init"
chmod 644 "$WORK_DIR/default.prop" "$WORK_DIR/dhclient.conf"

# --- 5. cpio + uimg をビルド ---
echo "[*] uimg をビルド中..."
mkdir -p "$(dirname "$OUT")"

MKIMAGE="${MKIMAGE:-$(command -v mkimage || true)}"

if [ -n "$MKIMAGE" ]; then
    # --- 5a. ホストの mkimage で直接ビルド ---
    echo "[*] mkimage: $MKIMAGE"
    # アーカイブ対象ディレクトリの外に出力する（内側だと自分自身を巻き込む）
    GZ="${WORK_DIR%/}.cpio.gz"
    rm -f "$GZ"
    (cd "$WORK_DIR" && find . | sort | cpio -o -H newc 2>/dev/null | gzip -9 > "$GZ")
    "$MKIMAGE" -A arm -O linux -T ramdisk -C gzip \
        -a 0x04000000 -e 0x04000000 \
        -n 'patched-initramfs' \
        -d "$GZ" \
        "$OUT"
    rm -f "$GZ"
elif command -v docker >/dev/null 2>&1; then
    # --- 5b. docker で cpio + uimg をビルド ---
    echo "[*] Docker で uimg をビルド中..."
    docker run --rm \
        -v "$WORK_DIR:/initramfs_work" \
        -v "$(dirname "$OUT"):/out" \
        ubuntu:22.04 bash -c "
apt-get update -qq && apt-get install -y -qq u-boot-tools cpio gzip 2>/dev/null
cd /initramfs_work
find . | sort | cpio -o -H newc 2>/dev/null | gzip -9 > /tmp/initramfs_patched.cpio.gz
mkimage -A arm -O linux -T ramdisk -C gzip \
    -a 0x04000000 -e 0x04000000 \
    -n 'patched-initramfs' \
    -d /tmp/initramfs_patched.cpio.gz \
    /out/initramfs_patched.uimg
echo '[+] ビルド完了'
ls -lh /out/initramfs_patched.uimg
"
else
    echo "[!] mkimage が見つかりません。'sudo apt install u-boot-tools' するか、docker を入れてください" >&2
    exit 1
fi

# Docker Desktop から WSL の /tmp 等が見えないと、空ディレクトリをマウントして
# 中身の無い uimg (数百バイト) が出来てしまう。書き込む前に気付けるよう検査する。
OUT_SIZE=$(stat -c %s "$OUT")
if [ "$OUT_SIZE" -lt 500000 ]; then
    echo "[!] $OUT が小さすぎます (${OUT_SIZE} bytes)。initramfs が空の可能性があります。" >&2
    echo "    docker から WORK_DIR ($WORK_DIR) が見えているか確認するか、MKIMAGE を指定してください。" >&2
    exit 1
fi

echo ""
echo "[+] 完了: $OUT"
