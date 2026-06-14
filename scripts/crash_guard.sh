#!/system/bin/sh
# crash_guard.sh — brick-prevention watchdog for SMB400 Mirakurun.
#
# Background:
#   On Android 8 a crashing process's own debuggerd handler forks crash_dump32.
#   Under memory pressure crash_dump32 itself crashes during ELF load, forking
#   another crash_dump32 — a self-sustaining fork bomb that exhausted kernel
#   slab/pagetables and bricked the device (required USB rescue boot).
#
# This watchdog runs OUTSIDE the Alpine chroot, in the Android shell, as a
# lightweight independent loop. It does two things every POLL seconds:
#   1. If crash_dump32 process count exceeds CD_MAX -> kill the chain + Node.
#   2. If MemAvailable drops below MEM_FLOOR_KB -> kill Node before OOM cascades.
#
# It deliberately does NOT disable crash_dump32 (so the first real crash still
# writes a tombstone for diagnosis). Killing Node is safe: start_mirakurun.sh's
# restart loop will relaunch it after a short delay. If Node immediately OOMs
# again this becomes a slow kill/restart loop — undesirable but NOT a brick.
#
# Run via: setsid sh /data/local/tmp/crash_guard.sh >> /data/local/tmp/crash_guard.log 2>&1 &
# (start_mirakurun.sh launches it automatically.)

POLL=1                 # seconds between checks (fast reaction to cascades)
CD_MAX=4               # max tolerated concurrent crash_dump32 processes
MEM_FLOOR_KB=500000    # kill Node if MemAvailable falls below this (kB) — large
                       # headroom so node dies long before exec starts failing
LOG=/data/local/tmp/crash_guard.log

# Try to give ourselves CPU priority so we still run under pressure.
renice -n -5 $$ 2>/dev/null || true

# Record our PID so start_mirakurun.sh can do a reliable singleton check
# (toybox `pgrep -f` self-matches its own argv, so a PID file is used instead).
echo $$ > /data/local/tmp/crash_guard.pid

echo "[crash_guard] started pid=$$ POLL=${POLL}s CD_MAX=${CD_MAX} MEM_FLOOR=${MEM_FLOOR_KB}kB" >> "$LOG"

while true; do
    # --- 1. crash_dump32 fork-bomb detection ---
    cd_count=$(pgrep crash_dump32 2>/dev/null | wc -l)
    if [ "$cd_count" -gt "$CD_MAX" ]; then
        echo "[crash_guard] crash_dump32 count=$cd_count > $CD_MAX — killing chain + Node" >> "$LOG"
        pkill -9 crash_dump32 2>/dev/null
        pkill -9 -f "lib/server.js" 2>/dev/null
        pkill -9 -f "Mirakurun:" 2>/dev/null
        pkill -9 node 2>/dev/null
    fi

    # --- 2. low-memory guard ---
    mem=$(grep MemAvailable /proc/meminfo 2>/dev/null | tr -dc '0-9')
    [ -z "$mem" ] && mem=999999
    if [ "$mem" -lt "$MEM_FLOOR_KB" ]; then
        echo "[crash_guard] MemAvailable=${mem}kB < ${MEM_FLOOR_KB}kB — killing Node to free memory" >> "$LOG"
        pkill -9 -f "lib/server.js" 2>/dev/null
        pkill -9 -f "Mirakurun:" 2>/dev/null
        pkill -9 node 2>/dev/null
        pkill -9 crash_dump32 2>/dev/null
    fi

    sleep "$POLL"
done
