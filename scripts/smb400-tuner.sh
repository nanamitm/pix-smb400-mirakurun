#!/system/bin/sh
# smb400-tuner.sh — tuner command wrapper for Mirakurun on SMB400.
#
# Usage (invoked by Mirakurun via tuners.yml):
#   smb400-tuner.sh <channel>
#
# Channel format determines operating mode:
#   GR:   integer 13-62    → tuner-stream-ng  (MPEG-TS, ISDB-T)
#   BS:   BSxx_y           → tuner-stream-bs  (MPEG-TS, ISDB-S)
#   BS4K: integer ≥40000   → tuner-stream-bs-ng | b61dec (descrambled TLV, ISDB-S3)
#
# BS4K descrambling note:
#   Scrambled TLV is unreadable by Mirakurun's TLVFilter → we descramble here,
#   inside the tuner command, before output.  tuners.yml sets tlvDecoder: null.
#   b61dec and tuner-stream-bs-ng run via chroot /proc/1/root to access the
#   Android linker paths required by libstationtv_*/libhi_msp.
#
# ACAS key (BS4K only): device-specific, never committed to git.
#   echo '<64-hex>' > /data/local/tmp/.acas_key ; chmod 600 /data/local/tmp/.acas_key

CHANNEL=$1
if [ -z "$CHANNEL" ]; then
    echo "smb400-tuner.sh: missing channel argument" >&2
    exit 1
fi

BINDIR=/data/local/tmp

case "$CHANNEL" in
    BS[0-9][0-9]_[0-9])
        # BS (ISDB-S, 従来2K): BSxx_y。xx=トランスポンダ番号で IF を算出。
        TPSTR=${CHANNEL#BS}       # "BS15_0" → "15_0"
        TPSTR=${TPSTR%_*}         # "15_0"   → "15"
        TP=$((TPSTR + 0))         # strip leading zero
        IF_KHZ=$((1049480 + (TP - 1) / 2 * 38360))
        # 1 トランスポンダに複数の相対 TS が多重されており、tunertest(mode=1) は
        # _y(相対インデックス)ではなく実 MPEG TS-ID で TS を選択する。全国共通の
        # BS TS-ID 表（実機 NIT から導出, 2026-06-21）で BSxx_y → TS-ID を引く。
        case "$CHANNEL" in
            BS01_0) TSID=16400 ;;  BS01_1) TSID=16401 ;;  BS01_2) TSID=16402 ;;
            BS03_0) TSID=16432 ;;
            BS05_0) TSID=17488 ;;
            BS09_0) TSID=16528 ;;  BS09_2) TSID=16530 ;;
            BS13_0) TSID=16592 ;;  BS13_1) TSID=16593 ;;  BS13_2) TSID=18130 ;;
            BS15_0) TSID=16625 ;;  BS15_2) TSID=18675 ;;
            BS19_0) TSID=18224 ;;
            BS21_0) TSID=18256 ;;
            BS23_0) TSID=18288 ;;  BS23_1) TSID=18801 ;;  BS23_3) TSID=18803 ;;
            *)      TSID=0 ;;      # 未知: 当該トランスポンダの先頭 TS を自動選択
        esac
        # 2K BS is MULTI2-scrambled (NHK / 有料局). Descramble in-command via the
        # ACAS chip (conventional/B-CAS CAS, APDU P2=0x02) so Mirakurun's TSFilter
        # receives plain MPEG-TS.  b21dec needs the Android linker/vendor libs, so
        # the pipe runs under chroot /proc/1/root (same as the BS4K path).  No key
        # arg: the chip holds the broadcaster work key (Kw) from prior live EMM.
        chroot /proc/1/root /system/bin/sh -c \
            "$BINDIR/tuner-stream-bs 0 1 $IF_KHZ $TSID | $BINDIR/b21dec" &
        CHROOT_PID=$!
        trap "pkill -f 'tuner-stream-bs 0 1 $IF_KHZ' 2>/dev/null; \
              pkill -f b21dec 2>/dev/null; \
              sleep 1; \
              kill -9 $CHROOT_PID 2>/dev/null; \
              pkill -9 -f 'tuner-stream-bs 0 1 $IF_KHZ' 2>/dev/null; \
              pkill -9 -f b21dec 2>/dev/null; \
              pkill -9 tunertest 2>/dev/null; exit 0" TERM INT
        wait $CHROOT_PID
        pkill -9 -f "tuner-stream-bs 0 1 $IF_KHZ" 2>/dev/null || true
        pkill -9 tunertest 2>/dev/null || true
        ;;
    4[0-9][0-9][0-9][0-9])
        # BS4K (ISDB-S3): 5-digit stream ID — static IF frequency lookup
        ACAS_KEY=$(cat /data/local/tmp/.acas_key 2>/dev/null | tr -d '[:space:]')
        if [ -z "$ACAS_KEY" ] || [ "${#ACAS_KEY}" -ne 64 ]; then
            echo "smb400-tuner.sh: ACAS key missing or invalid at /data/local/tmp/.acas_key" >&2
            exit 1
        fi
        case "$CHANNEL" in
            45168|45169|45170) IF_KHZ=1164560 ;;  # BS7  transponder (右旋)
            45232|45234)       IF_KHZ=1241280 ;;  # BS11 transponder (右旋)
            45328|45329|45330) IF_KHZ=1356360 ;;  # BS17 transponder (右旋)
            45280)             IF_KHZ=2472000 ;;  # BS8K (NHK BS8K, 左旋 11.97682GHz / IF 2472MHz, serviceId 102)
            *) echo "smb400-tuner.sh: unknown BS4K channel '$CHANNEL'" >&2; exit 1 ;;
        esac
        # Pipeline runs in host Android context (chroot to host root) so Android
        # shared libs are accessible.  Run in background so trap fires on SIGTERM.
        chroot /proc/1/root /system/bin/sh -c \
            "$BINDIR/tuner-stream-bs-ng 0 2 $IF_KHZ $CHANNEL | $BINDIR/b61dec -key $ACAS_KEY" &
        CHROOT_PID=$!
        trap "pkill -f 'tuner-stream-bs-ng 0 2 $IF_KHZ' 2>/dev/null; \
              pkill -f 'b61dec -key $ACAS_KEY' 2>/dev/null; \
              sleep 1; \
              kill -9 $CHROOT_PID 2>/dev/null; \
              pkill -9 -f 'tuner-stream-bs-ng 0 2 $IF_KHZ' 2>/dev/null; \
              pkill -9 -f 'b61dec -key $ACAS_KEY' 2>/dev/null; \
              pkill -9 tunertest 2>/dev/null; exit 0" TERM INT
        wait $CHROOT_PID
        # tuner-stream-bs-ng spawns tunertest as a child but does not kill it on exit
        pkill -9 -f "tuner-stream-bs-ng 0 2 $IF_KHZ" 2>/dev/null || true
        pkill -9 tunertest 2>/dev/null || true
        ;;
    [0-9]*)
        # GR (ISDB-T 地上波): 物理チャンネル 13-62。ifKHz = 473143 + (ch-13)*6000。
        IF_KHZ=$((473143 + (CHANNEL - 13) * 6000))
        # 地デジも MULTI2(B-CAS, CA_system_id 0x0005) スクランブルのため、2K BS と
        # 同じく b21dec で in-command 復号する。tuner-stream-ng は libhi_msp(DMX) を
        # dlopen し tunertest_oem を fork するため、b21dec ともども Android linker/
        # vendor lib を要する → chroot /proc/1/root 配下で実行 (BS4K 経路と同じ)。
        # ※ 地デジのワークキー Kw はチップが地上波 EMM 受信時に取得する。地上波を
        #   一度も受信していないチップでは ECM が視聴不可(rc≠0x0800)になり得る。
        chroot /proc/1/root /system/bin/sh -c \
            "$BINDIR/tuner-stream-ng 0 0 32 $IF_KHZ 6000 | $BINDIR/b21dec" &
        CHROOT_PID=$!
        # SIGTERM first so tuner-stream-ng can call dmx_deinit() — SIGKILL skips it and
        # leaves a stale DMX channel that blocks the next run.
        trap "pkill -f 'tuner-stream-ng 0 0 32 $IF_KHZ' 2>/dev/null; \
              pkill -f b21dec 2>/dev/null; \
              sleep 1; \
              kill -9 $CHROOT_PID 2>/dev/null; \
              pkill -9 -f 'tuner-stream-ng 0 0 32 $IF_KHZ' 2>/dev/null; \
              pkill -9 -f b21dec 2>/dev/null; \
              pkill -9 tunertest 2>/dev/null; exit 0" TERM INT
        wait $CHROOT_PID
        pkill -9 -f "tuner-stream-ng 0 0 32 $IF_KHZ" 2>/dev/null || true
        pkill -9 tunertest 2>/dev/null || true
        ;;
    *)
        echo "smb400-tuner.sh: unknown channel format '$CHANNEL'" >&2
        exit 1
        ;;
esac
