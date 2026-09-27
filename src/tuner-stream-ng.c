/*
 * tuner-stream-ng: PIX-SMB400 TS streamer using DMX direct capture.
 *
 * Startup strategy: ALWAYS COLD START (~2-4 s).
 *
 *   Fork tunertest_oem writing to a named FIFO.  Monitor the FIFO for a
 *   TS sync byte (0x47) to detect actual tuner lock — no fixed wait.
 *   SIGKILL tunertest_oem; GPIO/I2C state is preserved in hardware.
 *   Then set up DMX and stream.
 *
 * WARM START WAS REMOVED (2026-09; 5ch thread avi/1781912513 #589-622).
 *   The old warm start (~100 ms) assumed the tuner/demod still held the
 *   state of the last GR stream when .tuner_state matched.  But .tuner_state
 *   is written only by this GR binary: BS/CS/BS4K streams use the separate
 *   tuner-stream-bs / tuner-stream-bs-ng binaries, which never update it.
 *   (Reproduced on the device 2026-09-27: CS ND04 -> GR17 streamed only
 *   ~1.5 MB of garbage in 15 s until .tuner_state was removed.)  After a cross-band
 *   switch (GR -> BS/CS/BS4K) the demod is retuned while the state file
 *   still holds the old GR frequency.  A following GR stream with that
 *   frequency then ran the warm probe, saw "some data" from the wrong band
 *   and streamed garbage (no picture).  Cold start (~2-4 s) is slower but
 *   reliable, so warm start is gone for good.
 *
 * Note on why direct I2C cold start isn't used:
 *   The Hi3130E demodulator performs state-dependent calibration reads
 *   during initialization (reading chip-specific values and relaying them
 *   back via writes).  A static I2C log replay cannot replicate this.
 *   tunertest_oem handles this correctly via its internal SDK.
 *
 * Usage: tuner-stream-ng <tunerId> <dmxId> <portId> <freqKHz> <bwKHz>
 *
 * Build: see Makefile target build-tuner-stream-ng
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern int *__errno(void);
#undef  errno
#define errno (*__errno())
extern char *strerror(int errnum);

typedef int          HI_S32;
typedef unsigned int HI_U32;
typedef unsigned char HI_U8;
typedef void        *HI_HANDLE;

#define HI_SUCCESS  0
#define HI_FALSE    0

#define DMX_ID          0
#define TS_PORT_ID      32
#define REC_BUF_SIZE    (1024 * 1024)

/* Cold start: FIFO-based lock detection */
#define LOCK_FIFO_PATH    "/data/local/tmp/.tuner_lock_fifo"
#define LOCK_TIMEOUT_MS   12000
#define POST_KILL_MS      500   /* wait after SIGKILL for DMX driver cleanup */

/*===== Globals =====*/
static volatile int g_running = 1;
static pid_t g_tuner_pid = -1;
static int   g_fifo_fd   = -1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
    if (g_tuner_pid > 0) {
        kill(g_tuner_pid, SIGKILL);
        g_tuner_pid = -1;
    }
}

/*===== Library handles and function pointers =====*/

static void *g_lib_msp = NULL;

typedef HI_S32 (*PFN_DMX_Init)(void);
typedef HI_S32 (*PFN_DMX_DeInit)(void);
typedef HI_S32 (*PFN_DMX_AttachTSPort)(HI_U32, HI_U32);
typedef HI_S32 (*PFN_DMX_DetachTSPort)(HI_U32);
typedef HI_S32 (*PFN_DMX_CreateRecChn)(void *, HI_HANDLE *);
typedef HI_S32 (*PFN_DMX_DestroyRecChn)(HI_HANDLE);
typedef HI_S32 (*PFN_DMX_StartRecChn)(HI_HANDLE);
typedef HI_S32 (*PFN_DMX_StopRecChn)(HI_HANDLE);
typedef HI_S32 (*PFN_DMX_AcquireRecData)(HI_HANDLE, void *, HI_U32);
typedef HI_S32 (*PFN_DMX_ReleaseRecData)(HI_HANDLE, const void *);

typedef struct {
    HI_U8  *pDataAddr;
    HI_U32  u32DataPhyAddr;
    HI_U32  u32Len;
} HI_UNF_DMX_REC_DATA_S;

typedef struct {
    HI_U32 u32DmxId;
    HI_U32 u32RecBufSize;
    HI_U32 enRecType;
    HI_U32 bDescramed;
    HI_U32 enIndexType;
    HI_U32 u32IndexSrcPid;
    HI_U32 enTsPacketType;
} HI_UNF_DMX_REC_ATTR_S;

static PFN_DMX_Init          g_DMX_Init;
static PFN_DMX_DeInit        g_DMX_DeInit;
static PFN_DMX_AttachTSPort  g_DMX_AttachTSPort;
static PFN_DMX_DetachTSPort  g_DMX_DetachTSPort;
static PFN_DMX_CreateRecChn  g_DMX_CreateRecChn;
static PFN_DMX_DestroyRecChn g_DMX_DestroyRecChn;
static PFN_DMX_StartRecChn   g_DMX_StartRecChn;
static PFN_DMX_StopRecChn    g_DMX_StopRecChn;
static PFN_DMX_AcquireRecData  g_DMX_AcquireRecData;
static PFN_DMX_ReleaseRecData  g_DMX_ReleaseRecData;

static int load_libraries(void)
{
    g_lib_msp = dlopen("libhi_msp.so", RTLD_NOW);
    if (!g_lib_msp) {
        fprintf(stderr, "dlopen(libhi_msp.so): %s\n", dlerror());
        return -1;
    }

    g_DMX_Init           = dlsym(g_lib_msp, "HI_UNF_DMX_Init");
    g_DMX_DeInit         = dlsym(g_lib_msp, "HI_UNF_DMX_DeInit");
    g_DMX_AttachTSPort   = dlsym(g_lib_msp, "HI_UNF_DMX_AttachTSPort");
    g_DMX_DetachTSPort   = dlsym(g_lib_msp, "HI_UNF_DMX_DetachTSPort");
    g_DMX_CreateRecChn   = dlsym(g_lib_msp, "HI_UNF_DMX_CreateRecChn");
    g_DMX_DestroyRecChn  = dlsym(g_lib_msp, "HI_UNF_DMX_DestroyRecChn");
    g_DMX_StartRecChn    = dlsym(g_lib_msp, "HI_UNF_DMX_StartRecChn");
    g_DMX_StopRecChn     = dlsym(g_lib_msp, "HI_UNF_DMX_StopRecChn");
    g_DMX_AcquireRecData = dlsym(g_lib_msp, "HI_UNF_DMX_AcquireRecData");
    g_DMX_ReleaseRecData = dlsym(g_lib_msp, "HI_UNF_DMX_ReleaseRecData");

    if (!g_DMX_Init || !g_DMX_AttachTSPort || !g_DMX_CreateRecChn ||
        !g_DMX_StartRecChn || !g_DMX_AcquireRecData || !g_DMX_ReleaseRecData) {
        fprintf(stderr, "Failed to resolve DMX functions\n");
        return -1;
    }
    return 0;
}

/*===== DMX layer =====*/

static HI_S32 dmx_setup(HI_U32 dmx_id, HI_U32 port_id, HI_HANDLE *phRecChn)
{
    HI_S32 ret;
    HI_UNF_DMX_REC_ATTR_S attr;

    ret = g_DMX_Init();
    fprintf(stderr, "DMX_Init: ret=0x%x\n", ret);

    ret = g_DMX_AttachTSPort(dmx_id, port_id);
    fprintf(stderr, "DMX_AttachTSPort(dmx=%u, port=%u): ret=0x%x\n",
            dmx_id, port_id, ret);
    if (ret != HI_SUCCESS) return ret;

    memset(&attr, 0, sizeof(attr));
    attr.u32DmxId      = dmx_id;
    attr.u32RecBufSize = REC_BUF_SIZE;
    attr.enRecType     = 1;
    attr.bDescramed    = HI_FALSE;
    attr.enTsPacketType = 0;

    ret = g_DMX_CreateRecChn(&attr, phRecChn);
    fprintf(stderr, "DMX_CreateRecChn: ret=0x%x handle=%p\n", ret, (void *)*phRecChn);
    if (ret != HI_SUCCESS) return ret;

    ret = g_DMX_StartRecChn(*phRecChn);
    fprintf(stderr, "DMX_StartRecChn: ret=0x%x\n", ret);
    return ret;
}

static void dmx_deinit(HI_HANDLE hRecChn, HI_U32 dmx_id)
{
    if (g_DMX_StopRecChn)    g_DMX_StopRecChn(hRecChn);
    if (g_DMX_DestroyRecChn) g_DMX_DestroyRecChn(hRecChn);
    if (g_DMX_DetachTSPort)  g_DMX_DetachTSPort(dmx_id);
    if (g_DMX_DeInit)        g_DMX_DeInit();
}

static void dmx_stream(HI_HANDLE hRecChn, int ts_fd)
{
    HI_UNF_DMX_REC_DATA_S data;
    int locked = 0;
    int nloops = 0;
    int nerrors = 0;

    fprintf(stderr, "TS capture started\n");

    while (g_running) {
        nloops++;
        memset(&data, 0, sizeof(data));
        HI_S32 ret = g_DMX_AcquireRecData(hRecChn, &data, 1000);
        if (ret != HI_SUCCESS) {
            nerrors++;
            if (g_running) usleep(10000);
            continue;
        }
        if (data.u32Len == 0 || !data.pDataAddr) {
            g_DMX_ReleaseRecData(hRecChn, &data);
            continue;
        }
        if (!locked) {
            fprintf(stderr, "LOCKED (first TS: %u bytes)\n", data.u32Len);
            locked = 1;
        }
        const HI_U8 *p = data.pDataAddr;
        HI_U32 rem = data.u32Len;
        while (rem > 0 && g_running) {
            ssize_t w = write(ts_fd, p, (size_t)rem);
            if (w < 0) {
                fprintf(stderr, "write(ts_fd): errno=%d\n", errno);
                g_running = 0;
                break;
            }
            p   += w;
            rem -= (HI_U32)w;
        }
        g_DMX_ReleaseRecData(hRecChn, &data);
    }

    fprintf(stderr, "TS capture stopped: %d loops, %d errors\n", nloops, nerrors);
}

/*===== Cold start: tunertest_oem with FIFO lock detection =====*/

static pid_t start_tunertest_oem(int tuner_id, int freq_khz)
{
    unlink(LOCK_FIFO_PATH);

    if (mkfifo(LOCK_FIFO_PATH, 0600) < 0) {
        fprintf(stderr, "mkfifo(%s): %s\n", LOCK_FIFO_PATH, strerror(errno));
        return -1;
    }

    g_fifo_fd = open(LOCK_FIFO_PATH, O_RDONLY | O_NONBLOCK);
    if (g_fifo_fd < 0) {
        fprintf(stderr, "open(fifo,read): %s\n", strerror(errno));
        unlink(LOCK_FIFO_PATH);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork: %s\n", strerror(errno));
        close(g_fifo_fd); g_fifo_fd = -1;
        unlink(LOCK_FIFO_PATH);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        { int fd; for (fd = 3; fd < 1024; fd++) close(fd); }

        char tuner_str[8], freq_str[16];
        snprintf(tuner_str, sizeof(tuner_str), "%d", tuner_id);
        snprintf(freq_str,  sizeof(freq_str),  "%d", freq_khz);

        execl("/vendor/bin/tunertest_oem", "tunertest_oem",
              tuner_str,
              LOCK_FIFO_PATH,
              "2147483647",
              freq_str,
              "6",
              "1", "0", "1",
              NULL);
        _exit(1);
    }
    return pid;
}

static int wait_for_lock_fifo(void)
{
    unsigned char buf[4096];
    int elapsed_ms = 0;

    fprintf(stderr, "Waiting for tuner lock (max %dms)...\n", LOCK_TIMEOUT_MS);

    while (elapsed_ms < LOCK_TIMEOUT_MS && g_running) {
        ssize_t n = read(g_fifo_fd, buf, sizeof(buf));
        if (n > 0) {
            ssize_t i;
            for (i = 0; i < n; i++) {
                if (buf[i] == 0x47) {
                    fprintf(stderr, "Tuner locked at +%dms\n", elapsed_ms);
                    return 1;
                }
            }
        }
        usleep(50000);
        elapsed_ms += 50;
    }
    return 0;
}

/*===== Main =====*/

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr,
            "usage: %s <tunerId> <dmxId> <portId> <freqKHz> <bwKHz>\n",
            argv[0]);
        return 1;
    }

    int tuner_id = atoi(argv[1]);
    int dmx_id   = atoi(argv[2]);
    int port_id  = atoi(argv[3]);
    int freq_khz = atoi(argv[4]);
    int bw_khz   = atoi(argv[5]);

    fprintf(stderr, "tuner-stream-ng: tuner=%d dmx=%d port=%d freq=%d bw=%d kHz\n",
            tuner_id, dmx_id, port_id, freq_khz, bw_khz);

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, on_signal);
    signal(SIGCHLD, SIG_IGN);

    int ts_fd = dup(STDOUT_FILENO);
    if (ts_fd < 0) {
        fprintf(stderr, "dup(stdout): %s\n", strerror(errno));
        return 1;
    }
    {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); close(dn); }
    }

    if (load_libraries() != 0) return 1;

    HI_HANDLE hRecChn = NULL;

    /* ------------------------------------------------------------------ */
    /* COLD START: tunertest_oem + FIFO lock detection                    */
    /* Always cold start: warm start was removed (see header comment).    */
    /* ------------------------------------------------------------------ */
    if (!g_running) {
        dlclose(g_lib_msp);
        close(ts_fd);
        return 0;
    }

    fprintf(stderr, "Cold start: launching tunertest_oem for freq=%d kHz\n",
            freq_khz);
    g_tuner_pid = start_tunertest_oem(tuner_id, freq_khz);
    if (g_tuner_pid < 0) {
        fprintf(stderr, "Failed to start tunertest_oem\n");
        dlclose(g_lib_msp);
        close(ts_fd);
        return 1;
    }
    fprintf(stderr, "tunertest_oem pid=%d\n", (int)g_tuner_pid);

    int lock_ok = wait_for_lock_fifo();

    if (g_fifo_fd >= 0) { close(g_fifo_fd); g_fifo_fd = -1; }
    unlink(LOCK_FIFO_PATH);

    if (!g_running) {
        if (g_tuner_pid > 0) { kill(g_tuner_pid, SIGKILL); g_tuner_pid = -1; }
        dlclose(g_lib_msp);
        close(ts_fd);
        return 0;
    }
    if (!lock_ok)
        fprintf(stderr, "WARNING: lock timeout after %dms, proceeding anyway\n",
                LOCK_TIMEOUT_MS);

    fprintf(stderr, "Killing tunertest_oem pid=%d\n", (int)g_tuner_pid);
    kill(g_tuner_pid, SIGKILL);
    g_tuner_pid = -1;
    usleep(POST_KILL_MS * 1000);

    if (dmx_setup((HI_U32)dmx_id, (HI_U32)port_id, &hRecChn) != HI_SUCCESS) {
        fprintf(stderr, "DMX setup failed\n");
        dlclose(g_lib_msp);
        close(ts_fd);
        return 1;
    }

    dmx_stream(hRecChn, ts_fd);

    fprintf(stderr, "Cleaning up...\n");
    dmx_deinit(hRecChn, (HI_U32)dmx_id);
    close(ts_fd);
    dlclose(g_lib_msp);

    fprintf(stderr, "tuner-stream-ng: exit\n");
    return 0;
}
