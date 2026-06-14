#define _GNU_SOURCE
/*
 * tuner-stream-bs-ng: PIX-SMB400 satellite (BS/BS4K) streamer.
 *
 * Architecture:
 *   Uses pipe() instead of a named FIFO for tunertest output.
 *   tunertest writes to the pipe write-end; we read from the pipe read-end
 *   and relay to stdout.  Since a pipe has no file-size limit, tunertest
 *   never hits its 2 GB limit and runs indefinitely.
 *
 *   When the pipe write-end closes (tunertest exits for any reason),
 *   we restart tunertest with a new pipe.  During the ~5 s re-lock gap,
 *   keepalive null packets are sent to the HTTP client.
 *
 * Usage: tuner-stream-bs-ng <tunerId> <mode> <freqKHz> <streamId>
 */

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

extern int *__errno(void);
#undef  errno
#define errno (*__errno())
extern char *strerror(int errnum);

#define TS_SYNC 0x47u

static const uint8_t TS_NULL_PKT[188] = {
    0x47, 0x1F, 0xFF, 0x10,
};

static const uint8_t TLV_NULL_PKT[4] = {0x7F, 0xFF, 0x00, 0x00};

#define KEEPALIVE_INTERVAL_US  50000
#define READ_BUF_SIZE          (188 * 1024)
#define REPORT_INTERVAL_US     (5 * 1000000LL)

static volatile int g_running = 1;
static pid_t        g_child   = -1;
static int          g_pipe_rd = -1;

/*===== Logging =====*/

static void log_time(char *buf, int len)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm *tm = localtime(&tv.tv_sec);
    snprintf(buf, len, "%02d:%02d:%02d.%03ld",
             tm->tm_hour, tm->tm_min, tm->tm_sec, tv.tv_usec / 1000);
}

static void logm(const char *tag, const char *fmt, ...)
{
    char tbuf[16];
    log_time(tbuf, sizeof(tbuf));
    fprintf(stderr, "[%s][%s] ", tbuf, tag);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/*===== Signal =====*/

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_child > 0) kill(g_child, SIGTERM);
}

/*===== Helpers =====*/

static void send_keepalive(int ts_fd, int mode)
{
    if (mode == 2) {
        ssize_t w = write(ts_fd, TLV_NULL_PKT, sizeof(TLV_NULL_PKT));
        (void)w;
    } else {
        ssize_t w = write(ts_fd, TS_NULL_PKT, sizeof(TS_NULL_PKT));
        (void)w;
    }
}

static long long now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

/*
 * Fork tunertest with output directed to a pipe.
 * On success, *pipe_rd is set to the read-end fd.
 * Returns child pid, or -1 on error.
 */
static pid_t start_tunertest_pipe(int tuner_id, int mode, int freq_khz,
                                   int stream_id, int *pipe_rd)
{
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        logm("P", "pipe: %s\n", strerror(errno));
        return -1;
    }

    /* Increase pipe buffer to 1 MB to reduce blocking on tunertest write.
     * Default pipe buffer on Linux is 64 KB, which is too small for
     * high-bitrate TS streams (~4 MB/s). */
    int pipe_sz = fcntl(pipefd[1], F_SETPIPE_SZ, 1024 * 1024);
    if (pipe_sz < 0) {
        logm("P", "fcntl(F_SETPIPE_SZ): %s (using default)\n", strerror(errno));
    } else {
        logm("P", "pipe buffer: %d bytes\n", pipe_sz);
    }
    pid_t pid = fork();
    if (pid < 0) {
        logm("P", "fork: %s\n", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* Child: tunertest.
         *
         * IMPORTANT: tunertest writes diagnostic TEXT to its stdout and stderr.
         * If those share the same fd as the TS/TLV stream, the text gets
         * interleaved into the binary stream and corrupts it — b61dec then
         * loses TLV sync (0x7F) and descrambling breaks, so the client sees
         * only brief, intermittent playback.  Therefore we keep the stream on
         * a DEDICATED fd (3) and send tunertest's stdout/stderr to /dev/null.
         */
        close(pipefd[0]);

        /* Move the pipe write-end to fd 3; tunertest writes the stream there
         * via the /dev/fd/3 output path. */
        if (pipefd[1] != 3) {
            dup2(pipefd[1], 3);
            close(pipefd[1]);
        }

        /* Discard tunertest's text output so it never pollutes the stream. */
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        int din = open("/dev/null", O_RDONLY);
        if (din >= 0) { dup2(din, STDIN_FILENO); close(din); }
        { int fd; for (fd = 4; fd < 1024; fd++) close(fd); }

        char mode_str[4], freq_str[16], stream_str[16], tuner_str[4];
        char limit_str[24], lock_str[4];
        snprintf(mode_str,   sizeof(mode_str),   "%d", mode);
        snprintf(freq_str,   sizeof(freq_str),   "%d", freq_khz);
        snprintf(stream_str, sizeof(stream_str), "%d", stream_id);
        snprintf(tuner_str,  sizeof(tuner_str),  "%d", tuner_id);
        /* tunertest_oem's "limit size" arg is parsed with strtoll and the
         * bytes-written counter is a 64-bit accumulator (verified by
         * disassembling /vendor/bin/tunertest_oem), so it honours values far
         * beyond 4 GB.  A pipe has no file-size limit, so a huge 64-bit limit
         * effectively disables the periodic ~4 GB exit+restart (which caused a
         * ~3-4 s re-lock gap roughly every 17 min at BS4K bitrate).
         * ~9e18 bytes at broadcast bitrate ≈ never reached. */
        snprintf(limit_str,  sizeof(limit_str),  "%lld", 9000000000000000000LL);
        snprintf(lock_str,   sizeof(lock_str),   "%d", 0);

        /* Use /dev/fd/3 as output path — stream goes to fd 3 = pipe,
         * cleanly separated from tunertest's stdout/stderr text. */
        char *argv[] = {
            "/system/bin/tunertest",
            mode_str, "/dev/fd/3", limit_str, freq_str, stream_str,
            "1", tuner_str, lock_str, NULL
        };
        execv("/system/bin/tunertest", argv);
        _exit(1);
    }

    /* Parent */
    close(pipefd[1]);
    *pipe_rd = pipefd[0];
    return pid;
}

/*
 * Wait for satellite lock by reading from the pipe.
 * On lock, the bytes from the sync byte onward in the first chunk are
 * forwarded to ts_fd (so the initial TLV/MMT packets are not dropped).
 * Returns 1 on lock, 0 on failure.
 */
static int wait_for_lock(int pipe_rd, int ts_fd, int mode, pid_t child_pid)
{
    (void)child_pid;
    unsigned char buf[4096];
    int locked = 0;

    logm("P", "Waiting for satellite lock...\n");

    /* Lock = first valid sync byte appears on the stream fd:
     *   mode 1 (ISDB-S, MPEG-TS): 0x47
     *   mode 2 (ISDB-S3, TLV):    0x7F
     * The stream fd is now free of tunertest's text, so the sync byte is a
     * reliable lock signal (and we no longer drop real stream bytes by
     * treating arbitrary startup text as "locked"). */
    const uint8_t sync = (mode == 2) ? 0x7Fu : 0x47u;

    while (!locked && g_running) {
        ssize_t n = read(pipe_rd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t i;
            for (i = 0; i < n; i++) {
                if (buf[i] == sync) { locked = 1; break; }
            }
            if (locked) {
                logm("P", "LOCKED\n");
                /* Forward the initial chunk from the sync byte onward so the
                 * first TLV packet(s) reach the client. */
                const uint8_t *p = buf + i;
                ssize_t rem = n - i;
                while (rem > 0 && g_running) {
                    ssize_t w = write(ts_fd, p, (size_t)rem);
                    if (w < 0) {
                        if (errno == EINTR) continue;
                        break;
                    }
                    p += w; rem -= w;
                }
                return 1;
            }
        } else if (n == 0) {
            logm("P", "pipe closed before lock (tunertest exited)\n");
            return 0;
        } else {
            if (errno == EINTR) continue;
            logm("P", "read(pipe): errno=%d (%s)\n", errno, strerror(errno));
            return 0;
        }
    }
    return 0;
}

/*
 * Stream relay: read from pipe, write to ts_fd (stdout).
 */
static void stream_relay(int pipe_rd, int ts_fd, int mode)
{
    uint8_t *buf = (uint8_t *)malloc(READ_BUF_SIZE);
    if (!buf) return;

    unsigned long long total_bytes = 0;
    unsigned long long report_bytes = 0;
    long long report_due = now_us() + REPORT_INTERVAL_US;

    while (g_running) {
        ssize_t n = read(pipe_rd, buf, READ_BUF_SIZE);
        if (n <= 0) {
            if (n == 0) {
                logm("P", "EOF (tunertest exited)\n");
            } else if (errno == EINTR) {
                continue;
            } else {
                logm("P", "read(pipe): errno=%d (%s)\n", errno, strerror(errno));
            }
            break;
        }

        total_bytes  += (size_t)n;
        report_bytes += (size_t)n;

        const uint8_t *p = buf;
        ssize_t rem = n;
        while (rem > 0 && g_running) {
            ssize_t w = write(ts_fd, p, (size_t)rem);
            if (w < 0) {
                if (errno == EINTR) continue;
                logm("P", "write(ts_fd): errno=%d\n", errno);
                g_running = 0;
                break;
            }
            p   += w;
            rem -= w;
        }

        long long t = now_us();
        if (t >= report_due) {
            long long elapsed = t - (report_due - REPORT_INTERVAL_US);
            if (elapsed > 0) {
                unsigned long kbps = (unsigned long)(report_bytes * 1000000ULL / 1024 / elapsed);
                /* Mbps (decimal, 1 Mbit = 1e6 bits) = bytes*8 / elapsed_us.
                 * Scale by 100 to print two decimals: bytes*800 / elapsed_us. */
                unsigned long mbps_x100 = (unsigned long)(report_bytes * 800ULL / elapsed);
                logm("P", "throughput: %lu KB/s (%lu.%02lu Mbps), total: %llu MB\n",
                     kbps, mbps_x100 / 100, mbps_x100 % 100,
                     total_bytes / 1024 / 1024);
            }
            report_due = t + REPORT_INTERVAL_US;
            report_bytes = 0;
        }
    }

    free(buf);
}

/*===== Main =====*/

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "usage: %s <tunerId> <mode> <freqKHz> <streamId>\n"
            "  tunerId:  0 or 1\n"
            "  mode:     1=ISDB-S (BS), 2=ISDB-S3 (BS4K)\n"
            "  freqKHz:  IF frequency in kHz (sat_GHz*1e6 - 10678000)\n"
            "  streamId: 0=auto\n",
            argv[0]);
        return 1;
    }

    int tuner_id  = atoi(argv[1]);
    int mode      = atoi(argv[2]);
    int freq_khz  = atoi(argv[3]);
    int stream_id = atoi(argv[4]);

    if (mode != 1 && mode != 2) {
        logm("-", "error: mode must be 1 or 2, got %d\n", mode);
        return 1;
    }

    logm("-", "tuner-stream-bs-ng: tuner=%d mode=%d freq=%d streamId=%d (pipe)\n",
         tuner_id, mode, freq_khz, stream_id);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, on_signal);

    int ts_fd = dup(STDOUT_FILENO);
    if (ts_fd < 0) {
        logm("-", "dup(stdout): %s\n", strerror(errno));
        return 1;
    }
    {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
    }

    while (g_running) {
        g_pipe_rd = -1;
        g_child = start_tunertest_pipe(tuner_id, mode, freq_khz, stream_id,
                                        &g_pipe_rd);
        if (g_child < 0 || g_pipe_rd < 0) {
            logm("-", "failed to start tunertest, retrying in 3s\n");
            if (g_pipe_rd >= 0) close(g_pipe_rd);
            if (g_child > 0) { kill(g_child, SIGTERM); waitpid(g_child, NULL, 0); }
            g_pipe_rd = -1;
            g_child = -1;
            sleep(3);
            continue;
        }

        logm("P", "tunertest pid=%d, pipe_rd=%d\n", (int)g_child, g_pipe_rd);

        if (!wait_for_lock(g_pipe_rd, ts_fd, mode, g_child)) {
            logm("P", "lock failed, restarting\n");
            close(g_pipe_rd); g_pipe_rd = -1;
            kill(g_child, SIGTERM); waitpid(g_child, NULL, 0); g_child = -1;
            sleep(1);
            continue;
        }

        stream_relay(g_pipe_rd, ts_fd, mode);

        close(g_pipe_rd); g_pipe_rd = -1;

        int status = 0;
        if (g_child > 0) {
            waitpid(g_child, &status, 0);
            g_child = -1;
        }

        if (!g_running) break;

        if (WIFEXITED(status))
            logm("P", "tunertest exited (code=%d), restarting...\n", WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            logm("P", "tunertest killed (signal=%d), restarting...\n", WTERMSIG(status));
        else
            logm("P", "tunertest exited (status=%d), restarting...\n", status);

        sleep(1);
    }

    if (g_pipe_rd >= 0) close(g_pipe_rd);
    if (g_child > 0) { kill(g_child, SIGTERM); waitpid(g_child, NULL, 0); }
    close(ts_fd);

    logm("-", "tuner-stream-bs-ng: exit\n");
    return 0;
}
