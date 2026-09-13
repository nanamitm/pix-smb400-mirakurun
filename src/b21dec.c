/*
 * b21dec.c — Conventional 2K BS/CS (ISDB-S, ARIB STD-B25 / B-CAS) MPEG-TS
 *            descrambler for PIX-SMB400, using the on-device ACAS chip.
 *
 * Reads a scrambled MPEG-TS stream from stdin (tuner-stream-bs mode=1 output),
 * parses PSI (PAT/PMT) to locate ECM PIDs, sends each ECM to the ACAS chip
 * (in conventional/B-CAS "ACAS mode", APDU P2=0x02) to obtain MULTI2 scramble
 * keys, descrambles payloads with MULTI2, and writes clean MPEG-TS to stdout.
 *
 *   tuner-stream-bs 0 1 <IF_kHz> 0 | b21dec
 *
 * Unlike b61dec (BS4K / ARIB STD-B61 / ACAS-RMP / AES-128-CTR), this path uses
 * the chip's *conventional* CAS function:
 *   INIT  90 30 00 02 00 -> system_key=resp[16:48], init_cbc=resp[48:56],
 *                           ca_system_id=be16(resp+6), rc=be16(resp+4)==0x2100
 *   ECM   90 34 00 02 Lc <body> 00 -> Ks=resp[6:22] (odd[0:8]+even[8:16]),
 *                           rc=be16(resp+4) (0x0800=viewable)
 * No master key is required: the chip holds the broadcaster work key (Kw),
 * provisioned via EMM during prior live reception.
 *
 * The MULTI2 core below is an embedded scalar copy of libaribb25's multi2.c
 * (multi2_simd.h pulls x86 intrinsics so the upstream files don't cross-compile
 * to ARM; only the scalar core is needed here).
 *
 * Build (Android ARM32): see Makefile target build-b21dec
 */
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>

extern int *__errno(void);
#undef  errno
#define errno (*__errno())
extern char *strerror(int errnum);

/* ===================================================================
 * MULTI2 scalar core  (from libaribb25 multi2.c, scalar path only)
 * =================================================================== */
typedef struct { uint32_t key[8]; } CORE_PARAM;
typedef struct { uint32_t r; uint32_t l; } CORE_DATA;

static inline uint32_t rol32(uint32_t v, uint32_t c) { return (v << c) | (v >> (32 - c)); }
static inline uint32_t ld_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint8_t *st_be32(uint8_t *d, uint32_t v) {
    d[0] = v >> 24; d[1] = v >> 16; d[2] = v >> 8; d[3] = v; return d + 4;
}

static void core_pi1(CORE_DATA *dst, CORE_DATA *src) {
    dst->l = src->l;
    dst->r = src->r ^ src->l;
}
static void core_pi2(CORE_DATA *dst, CORE_DATA *src, uint32_t a) {
    uint32_t t0 = src->r + a;
    uint32_t t1 = rol32(t0, 1) + t0 - 1;
    uint32_t t2 = rol32(t1, 4) ^ t1;
    dst->l = src->l ^ t2;
    dst->r = src->r;
}
static void core_pi3(CORE_DATA *dst, CORE_DATA *src, uint32_t a, uint32_t b) {
    uint32_t t0 = src->l + a;
    uint32_t t1 = rol32(t0, 2) + t0 + 1;
    uint32_t t2 = rol32(t1, 8) ^ t1;
    uint32_t t3 = t2 + b;
    uint32_t t4 = rol32(t3, 1) - t3;
    uint32_t t5 = rol32(t4, 16) ^ (t4 | src->l);
    dst->l = src->l;
    dst->r = src->r ^ t5;
}
static void core_pi4(CORE_DATA *dst, CORE_DATA *src, uint32_t a) {
    uint32_t t0 = src->r + a;
    uint32_t t1 = rol32(t0, 2) + t0 + 1;
    dst->l = src->l ^ t1;
    dst->r = src->r;
}
static void core_schedule(CORE_PARAM *work, CORE_PARAM *skey, CORE_DATA *dkey) {
    CORE_DATA b1, b2, b3, b4, b5, b6, b7, b8, b9;
    core_pi1(&b1, dkey);
    core_pi2(&b2, &b1, skey->key[0]); work->key[0] = b2.l;
    core_pi3(&b3, &b2, skey->key[1], skey->key[2]); work->key[1] = b3.r;
    core_pi4(&b4, &b3, skey->key[3]); work->key[2] = b4.l;
    core_pi1(&b5, &b4); work->key[3] = b5.r;
    core_pi2(&b6, &b5, skey->key[4]); work->key[4] = b6.l;
    core_pi3(&b7, &b6, skey->key[5], skey->key[6]); work->key[5] = b7.r;
    core_pi4(&b8, &b7, skey->key[7]); work->key[6] = b8.l;
    core_pi1(&b9, &b8); work->key[7] = b9.r;
}
static void core_encrypt(CORE_DATA *dst, CORE_DATA *src, CORE_PARAM *w, int32_t round) {
    CORE_DATA tmp;
    dst->l = src->l; dst->r = src->r;
    for (int32_t i = 0; i < round; i++) {
        core_pi1(&tmp, dst);  core_pi2(dst, &tmp, w->key[0]);
        core_pi3(&tmp, dst, w->key[1], w->key[2]); core_pi4(dst, &tmp, w->key[3]);
        core_pi1(&tmp, dst);  core_pi2(dst, &tmp, w->key[4]);
        core_pi3(&tmp, dst, w->key[5], w->key[6]); core_pi4(dst, &tmp, w->key[7]);
    }
}
static void core_decrypt(CORE_DATA *dst, CORE_DATA *src, CORE_PARAM *w, int32_t round) {
    CORE_DATA tmp;
    dst->l = src->l; dst->r = src->r;
    for (int32_t i = 0; i < round; i++) {
        core_pi4(&tmp, dst, w->key[7]); core_pi3(dst, &tmp, w->key[5], w->key[6]);
        core_pi2(&tmp, dst, w->key[4]); core_pi1(dst, &tmp);
        core_pi4(&tmp, dst, w->key[3]); core_pi3(dst, &tmp, w->key[1], w->key[2]);
        core_pi2(&tmp, dst, w->key[0]); core_pi1(dst, &tmp);
    }
}

/* MULTI2 descrambler context (round 4). */
typedef struct {
    CORE_PARAM sys;        /* system key (card-level)        */
    CORE_DATA  cbc_init;   /* CBC IV (card-level)            */
    CORE_PARAM wrk[2];     /* work keys 0:odd 1:even         */
    int        round;
    int        have_sys;
    int        have_keys;
} M2;

static void m2_set_system_key(M2 *m, const uint8_t v[32]) {
    for (int i = 0; i < 8; i++) m->sys.key[i] = ld_be32(v + i * 4);
    m->have_sys = 1;
}
static void m2_set_init_cbc(M2 *m, const uint8_t v[8]) {
    m->cbc_init.l = ld_be32(v + 0);
    m->cbc_init.r = ld_be32(v + 4);
}
/* ks16 = odd[0:8] | even[8:16] */
static void m2_set_scramble_key(M2 *m, const uint8_t ks16[16]) {
    CORE_DATA scr0, scr1;
    scr0.l = ld_be32(ks16 + 0);  scr0.r = ld_be32(ks16 + 4);
    scr1.l = ld_be32(ks16 + 8);  scr1.r = ld_be32(ks16 + 12);
    core_schedule(&m->wrk[0], &m->sys, &scr0);
    core_schedule(&m->wrk[1], &m->sys, &scr1);
    m->have_keys = 1;
}
/* In-place MULTI2 decrypt of a TS payload. type = transport_scrambling_control
 * (0x02 = even key, 0x03 = odd key). Mirrors libaribb25 decrypt_multi2. */
static void m2_decrypt(M2 *m, int type, uint8_t *buf, int size) {
    CORE_DATA src, dst, cbc;
    CORE_PARAM *prm = (type == 0x02) ? &m->wrk[1] : &m->wrk[0];
    uint8_t *p = buf;
    cbc.l = m->cbc_init.l; cbc.r = m->cbc_init.r;
    while (size >= 8) {
        src.l = ld_be32(p + 0); src.r = ld_be32(p + 4);
        core_decrypt(&dst, &src, prm, m->round);
        dst.l ^= cbc.l; dst.r ^= cbc.r;
        cbc.l = src.l; cbc.r = src.r;
        p = st_be32(p, dst.l); p = st_be32(p, dst.r);
        size -= 8;
    }
    if (size > 0) {
        uint8_t tmp[8];
        core_encrypt(&dst, &cbc, prm, m->round);
        st_be32(tmp + 0, dst.l); st_be32(tmp + 4, dst.r);
        for (int i = 0; i < size; i++) p[i] ^= tmp[i];
    }
}

/* ===================================================================
 * ACAS chip interface via SCI_WRAPPER (libstationtv_lt_px_stream.so)
 * Conventional / B-CAS command set, ACAS mode (P2 = 0x02).
 * =================================================================== */
static void *g_lib;
typedef int (*fn0)(void);
typedef int (*set_fn)(uint8_t *, int, uint8_t *, int *);
typedef int (*get_fn)(uint8_t *, int *);
static fn0    sw_InitAll, sw_ResetF, sw_WaitAct;
static set_fn sw_SetData;
static get_fn sw_GetData, sw_GetAtr;

static int sci_load(void) {
    g_lib = dlopen("/vendor/lib/libstationtv_lt_px_stream.so", RTLD_NOW);
    if (!g_lib) { fprintf(stderr, "b21dec: dlopen: %s\n", dlerror()); return -1; }
#define LD(v,s) v = dlsym(g_lib,s); if(!v){fprintf(stderr,"b21dec: dlsym %s: %s\n",s,dlerror());return -1;}
    LD(sw_InitAll, "SCI_WRAPPER_InitForAllProcess")
    LD(sw_ResetF,  "SCI_WRAPPER_ResetForced")
    LD(sw_WaitAct, "SCI_WRAPPER_WaitForActivation")
    LD(sw_SetData, "SCI_WRAPPER_SetData")
    LD(sw_GetData, "SCI_WRAPPER_GetData")
    LD(sw_GetAtr,  "SCI_WRAPPER_GetAtr")
#undef LD
    return 0;
}

static int acas_exchange(const uint8_t *cmd, int clen, uint8_t *resp, int *rlen) {
    uint8_t dummy[4]; int dl = 0;
    int r = sw_SetData((uint8_t *)cmd, clen, dummy, &dl);
    if (r != 0) { fprintf(stderr, "b21dec: SetData err=%d\n", r); return -1; }
    *rlen = 256;
    r = sw_GetData(resp, rlen);
    if (r != 0) { fprintf(stderr, "b21dec: GetData err=%d\n", r); return -1; }
    return 0;
}

/* Conventional INIT (INS=0x30, ACAS P2=0x02): fetch system key + CBC IV. */
static int acas_bcas_init(M2 *m) {
    static const uint8_t cmd[5] = { 0x90, 0x30, 0x00, 0x02, 0x00 };
    uint8_t resp[256]; int rlen;
    if (acas_exchange(cmd, 5, resp, &rlen) < 0) return -1;
    if (rlen < 57) { fprintf(stderr, "b21dec: INIT short resp (%d)\n", rlen); return -1; }
    int rc = (resp[4] << 8) | resp[5];
    int casid = (resp[6] << 8) | resp[7];
    if (rc != 0x2100) { fprintf(stderr, "b21dec: INIT rc=0x%04x (want 0x2100)\n", rc); return -1; }
    m2_set_system_key(m, resp + 16);
    m2_set_init_cbc(m, resp + 48);
    fprintf(stderr, "b21dec: ACAS conventional init OK (ca_system_id=0x%04x)\n", casid);
    return 0;
}

/* ECM request (INS=0x34, ACAS P2=0x02). body = ECM section[8 : len-4].
 * On success fills ks16 (odd[0:8]+even[8:16]) and returns return_code. */
static int acas_bcas_ecm(const uint8_t *body, int blen, uint8_t ks16[16]) {
    if (blen < 1 || blen > 251) return -1;
    uint8_t cmd[5 + 251 + 1];
    cmd[0] = 0x90; cmd[1] = 0x34; cmd[2] = 0x00; cmd[3] = 0x02; cmd[4] = (uint8_t)blen;
    memcpy(cmd + 5, body, blen);
    cmd[5 + blen] = 0x00;
    uint8_t resp[256]; int rlen;
    if (acas_exchange(cmd, 6 + blen, resp, &rlen) < 0) return -1;
    if (rlen < 25) { fprintf(stderr, "b21dec: ECM short resp (%d)\n", rlen); return -1; }
    int rc = (resp[4] << 8) | resp[5];
    memcpy(ks16, resp + 6, 16);
    return rc;
}

/* ===================================================================
 * MPEG-TS / PSI processing
 * =================================================================== */
#define TS_PKT 188
#define TS_SYNC 0x47
#define NPID 8192

/* Per-ECM-PID key state. */
#define MAX_ECM 8
typedef struct {
    uint16_t pid;
    int      used;
    uint8_t  cache[256];   /* last ECM body (dedup) */
    int      cache_len;
    M2       m2;           /* shares sys/cbc, holds its own work keys */
} EcmSlot;

static EcmSlot  g_ecm[MAX_ECM];
static int16_t  g_pid_ecm[NPID];   /* data PID -> ecm slot index, or -1 */
static uint8_t  g_is_pmt[NPID];    /* 1 if PID carries a PMT           */
static uint8_t  g_is_ecm[NPID];    /* 1 if PID carries ECM             */
static CORE_PARAM g_sys; static CORE_DATA g_cbc; static int g_card_ready;

static long g_stat_pkts, g_stat_scrambled, g_stat_descrambled, g_stat_ecm_calls;

static int find_or_make_ecm_slot(uint16_t pid) {
    for (int i = 0; i < MAX_ECM; i++)
        if (g_ecm[i].used && g_ecm[i].pid == pid) return i;
    for (int i = 0; i < MAX_ECM; i++)
        if (!g_ecm[i].used) {
            g_ecm[i].used = 1; g_ecm[i].pid = pid; g_ecm[i].cache_len = 0;
            g_ecm[i].m2 = (M2){0}; g_ecm[i].m2.round = 4;
            g_ecm[i].m2.sys = g_sys; g_ecm[i].m2.cbc_init = g_cbc; g_ecm[i].m2.have_sys = 1;
            g_is_ecm[pid] = 1;
            return i;
        }
    return -1;
}

/* Parse one single-packet PSI section payload; returns section start or NULL. */
static const uint8_t *psi_section(const uint8_t *pkt, int *seclen_out) {
    int afc = (pkt[3] >> 4) & 0x3;
    int off = 4;
    if (afc & 0x2) off = 5 + pkt[4];
    if (off >= TS_PKT) return NULL;
    int ptr = pkt[off];
    int s = off + 1 + ptr;
    if (s + 3 > TS_PKT) return NULL;
    const uint8_t *sec = pkt + s;
    int seclen = ((sec[1] & 0x0f) << 8) | sec[2];
    if (s + 3 + seclen > TS_PKT) { /* spans packets: parse what we can up to pkt end */
        seclen = TS_PKT - s - 3;
    }
    *seclen_out = seclen;
    return sec;
}

static void parse_pat(const uint8_t *pkt) {
    int seclen; const uint8_t *sec = psi_section(pkt, &seclen);
    if (!sec || sec[0] != 0x00) return;
    const uint8_t *body = sec + 8;
    int blen = seclen - 5 - 4; /* section_length - 5(hdr after len) - 4(crc) */
    for (int i = 0; i + 4 <= blen; i += 4) {
        int prog = (body[i] << 8) | body[i + 1];
        int pmt  = ((body[i + 2] & 0x1f) << 8) | body[i + 3];
        if (prog != 0 && pmt < NPID) g_is_pmt[pmt] = 1;
    }
}

/* Returns ECM PID from a CA_descriptor loop, or -1 if none. */
static int scan_ca_desc(const uint8_t *d, int len) {
    int k = 0;
    while (k + 2 <= len) {
        int tag = d[k], dl = d[k + 1];
        if (tag == 0x09 && dl >= 4) {
            int ecm_pid = ((d[k + 4] & 0x1f) << 8) | d[k + 5];
            if (ecm_pid != 0x1fff) return ecm_pid;
        }
        k += 2 + dl;
    }
    return -1;
}

static void parse_pmt_sec(const uint8_t *sec, int seclen) {
    if (!sec || sec[0] != 0x02) return;
    int total = seclen - 4;          /* exclude CRC; relative to sec+3 */
    int pil = ((sec[10] & 0x0f) << 8) | sec[11];
    int prog_ecm = scan_ca_desc(sec + 12, pil);
    if (prog_ecm >= 0) find_or_make_ecm_slot((uint16_t)prog_ecm);
    int es = 12 + pil;
    while (es + 5 <= total + 3) {
        int epid = ((sec[es + 1] & 0x1f) << 8) | sec[es + 2];
        int il   = ((sec[es + 3] & 0x0f) << 8) | sec[es + 4];
        int es_ecm = scan_ca_desc(sec + es + 5, il);
        int ecm_pid = (es_ecm >= 0) ? es_ecm : prog_ecm;
        if (epid < NPID && ecm_pid >= 0) {
            int slot = find_or_make_ecm_slot((uint16_t)ecm_pid);
            if (slot >= 0) g_pid_ecm[epid] = (int16_t)slot;
        }
        es += 5 + il;
    }
}

/* Multi-packet PSI section assembler (for PMT).
 * NHK-G's PMT is 210 bytes and spans 2 TS packets; the old single-packet
 * parse silently dropped ES entries in the continuation fragment (e.g. data
 * PIDs 369/370), leaving them scrambled. Assemble per PID, then parse. */
#define SEC_MAX 1024
#define NSECASM 16
typedef struct { uint16_t pid; uint16_t have; uint16_t total; uint8_t cc; uint8_t active; uint8_t buf[SEC_MAX]; } SecAsm;
static SecAsm g_secasm[NSECASM];

static void feed_pmt(const uint8_t *pkt) {
    int pid  = ((pkt[1] & 0x1f) << 8) | pkt[2];
    int pusi = (pkt[1] >> 6) & 1;
    int cc   = pkt[3] & 0x0f;
    int afc  = (pkt[3] >> 4) & 0x3;
    if (!(afc & 0x1)) return;                       /* no payload (CC not incremented) */
    int off  = 4;
    if (afc & 0x2) off = 5 + pkt[4];
    if (off >= TS_PKT) return;
    const uint8_t *pay = pkt + off;
    int paylen = TS_PKT - off;

    SecAsm *a = NULL;
    for (int i = 0; i < NSECASM; i++)
        if (g_secasm[i].active && g_secasm[i].pid == pid) { a = &g_secasm[i]; break; }
    if (pusi) {
        int ptr = pay[0];
        if (1 + ptr + 3 > paylen) return;
        const uint8_t *sec = pay + 1 + ptr;
        int seclen = ((sec[1] & 0x0f) << 8) | sec[2];
        int total = 3 + seclen;
        if (total > SEC_MAX) return;               /* absurd: ignore */
        if (!a) {
            for (int i = 0; i < NSECASM; i++)
                if (!g_secasm[i].active) { a = &g_secasm[i]; break; }
            if (!a) return;
            a->pid = (uint16_t)pid; a->active = 1;
        }
        int take = total < paylen - 1 - ptr ? total : paylen - 1 - ptr;
        memcpy(a->buf, sec, take);
        a->have = (uint16_t)take; a->total = (uint16_t)total; a->cc = (uint8_t)cc;
        if (a->have >= a->total) { a->active = 0; parse_pmt_sec(a->buf, (int)(a->total - 3)); }
        return;
    }
    if (!a) return;                                 /* no assembly in flight */
    if (cc == a->cc) return;                        /* duplicate packet: ignore */
    if (((a->cc + 1) & 0x0f) != cc) { a->active = 0; return; }  /* gap: drop */
    int need = (int)a->total - (int)a->have;
    int take = need < paylen ? need : paylen;
    memcpy(a->buf + a->have, pay, take);
    a->have += (uint16_t)take; a->cc = (uint8_t)cc;
    if (a->have >= a->total) { a->active = 0; parse_pmt_sec(a->buf, (int)(a->total - 3)); }
}

/* Process an ECM section: send to chip if changed, update slot's work keys. */
static void process_ecm(int slot, const uint8_t *pkt) {
    int seclen; const uint8_t *sec = psi_section(pkt, &seclen);
    if (!sec || (sec[0] != 0x82 && sec[0] != 0x83)) return;
    int total = 3 + seclen;
    if (total < 12) return;
    const uint8_t *body = sec + 8;       /* long-form section header = 8 bytes */
    int blen = total - 8 - 4;            /* minus header and CRC               */
    if (blen < 1 || blen > 200) return;
    EcmSlot *e = &g_ecm[slot];
    if (e->cache_len == blen && memcmp(e->cache, body, blen) == 0) return; /* dedup */
    uint8_t ks16[16];
    int rc = acas_bcas_ecm(body, blen, ks16);
    g_stat_ecm_calls++;
    if (rc < 0) return;
    if (rc != 0x0800) {
        fprintf(stderr, "b21dec: ECM pid=0x%04x rc=0x%04x (not viewable)\n", e->pid, rc);
        /* keep previous keys; cache to avoid hammering the chip */
    } else {
        m2_set_scramble_key(&e->m2, ks16);
    }
    memcpy(e->cache, body, blen); e->cache_len = blen;
}

static volatile int g_running = 1;
static void on_sig(int s) { (void)s; g_running = 0; }

/* Descramble (if needed) and write one TS packet to stdout. */
static void emit_packet(uint8_t *pkt) {
    int pid = ((pkt[1] & 0x1f) << 8) | pkt[2];
    int tsc = (pkt[3] >> 6) & 3;
    if (tsc >= 2) {
        g_stat_scrambled++;
        int slot = (pid < NPID) ? g_pid_ecm[pid] : -1;
        if (slot >= 0 && g_ecm[slot].m2.have_keys) {
            int afc = (pkt[3] >> 4) & 3;
            int off = 4;
            if (afc & 2) off = 5 + pkt[4];
            if (off < TS_PKT) {
                m2_decrypt(&g_ecm[slot].m2, tsc, pkt + off, TS_PKT - off);
                pkt[3] &= 0x3f;          /* clear transport_scrambling_control */
                g_stat_descrambled++;
            }
        }
        /* else: keys not ready — pass through */
    }
    ssize_t w = 0;
    while (w < TS_PKT) {
        ssize_t k = write(STDOUT_FILENO, pkt + w, TS_PKT - w);
        if (k <= 0) { g_running = 0; break; }
        w += k;
    }
}

static int any_keys_ready(void) {
    for (int s = 0; s < MAX_ECM; s++)
        if (g_ecm[s].used && g_ecm[s].m2.have_keys) return 1;
    return 0;
}

/* Startup hold: buffer packets until the first ECM key is acquired, so the
 * stream begins fully descrambled instead of leaking ~0.3s of scrambled data.
 * Capped so FTA / unpurchased channels still start promptly (pass-through). */
#define MAX_PEND 8192
static uint8_t g_pend[MAX_PEND][TS_PKT];
static int     g_pend_n = 0;
static int     g_hold = 1;

static void flush_pending(void) {
    for (int i = 0; i < g_pend_n && g_running; i++) emit_packet(g_pend[i]);
    g_pend_n = 0;
    g_hold = 0;
}

static void print_stats(void) {
    fprintf(stderr, "b21dec: pkts=%ld scrambled=%ld descrambled=%ld ecm_calls=%ld\n",
            g_stat_pkts, g_stat_scrambled, g_stat_descrambled, g_stat_ecm_calls);
}

int main(int argc, char **argv) {
    int verbose = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-v")) verbose = 1;

    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGPIPE, on_sig);

    for (int i = 0; i < NPID; i++) g_pid_ecm[i] = -1;

    if (sci_load() < 0) return 1;
    int r;
    if ((r = sw_InitAll()) != 0) { fprintf(stderr, "b21dec: InitForAllProcess=%d\n", r); return 1; }
    if ((r = sw_ResetF())  != 0) { fprintf(stderr, "b21dec: ResetForced=%d\n", r); return 1; }
    if ((r = sw_WaitAct()) != 0) { fprintf(stderr, "b21dec: WaitForActivation=%d\n", r); return 1; }

    M2 cardm = {0}; cardm.round = 4;
    if (acas_bcas_init(&cardm) < 0) return 1;
    g_sys = cardm.sys; g_cbc = cardm.cbc_init; g_card_ready = 1;

    /* Streaming: read TS, resync on 0x47, parse PSI, descramble. */
    uint8_t buf[TS_PKT * 256];
    uint8_t pkt[TS_PKT];
    int held = 0;                 /* bytes held in pkt[] across reads */

    while (g_running) {
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) break;
        ssize_t i = 0;
        while (i < n) {
            /* assemble one 188-byte packet (handles split reads) */
            int need = TS_PKT - held;
            int avail = (int)(n - i);
            int take = avail < need ? avail : need;
            memcpy(pkt + held, buf + i, take);
            held += take; i += take;
            if (held < TS_PKT) break;
            held = 0;

            if (pkt[0] != TS_SYNC) {
                /* resync: shift to next 0x47 within this packet window */
                int j = 1;
                while (j < TS_PKT && pkt[j] != TS_SYNC) j++;
                if (j < TS_PKT) { memmove(pkt, pkt + j, TS_PKT - j); held = TS_PKT - j; }
                continue;
            }

            g_stat_pkts++;
            int pid  = ((pkt[1] & 0x1f) << 8) | pkt[2];
            int pusi = (pkt[1] >> 6) & 1;
            int tsc  = (pkt[3] >> 6) & 3;

            if (pid == 0x0000 && pusi)            parse_pat(pkt);
            else if (pid < NPID && g_is_pmt[pid]) feed_pmt(pkt);  /* PUSI + continuations */
            if (pid < NPID && g_is_ecm[pid] && pusi) {
                for (int s = 0; s < MAX_ECM; s++)
                    if (g_ecm[s].used && g_ecm[s].pid == pid) { process_ecm(s, pkt); break; }
            }

            (void)tsc;
            if (g_hold) {
                if (any_keys_ready()) {
                    flush_pending();        /* keys ready: descramble held packets */
                    emit_packet(pkt);
                } else if (g_pend_n < MAX_PEND) {
                    memcpy(g_pend[g_pend_n++], pkt, TS_PKT);
                } else {
                    flush_pending();        /* cap reached (FTA/unpurchased): give up holding */
                    emit_packet(pkt);
                }
            } else {
                emit_packet(pkt);
            }
        }
    }

    if (g_pend_n > 0) flush_pending();   /* EOF while still holding */

    if (verbose) print_stats();
    return 0;
}
