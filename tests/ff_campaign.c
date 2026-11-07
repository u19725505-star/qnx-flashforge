/* ff_campaign - power-cut campaign.
 * Cuts the power at every program operation of a workload, with 16 different tear patterns each,
 * then recovers and checks three designs of a tiny append-only record store:
 *   A  naive: a record is valid if its first field is not erased
 *   B  CRC-checked, but reuses a torn slot for the next record
 *   C  CRC-checked, and skips torn slots
 * Prints machine-readable lines (FFCAMP...) that the notebook turns into tables and charts. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include "ff_ioctl.h"

#define NREC   24u
#define NPOST  4u
#define SEEDS  16u
#define SLOT   32u
#define SLOTS  (FF_SECTOR_BYTES / SLOT)

enum { D_NAIVE = 0, D_CRC_REUSE = 1, D_SAFE = 2, NDESIGN = 3 };
enum { RES_OK = 0, RES_LOST = 1, RES_CORRUPT = 2 };

static const char  DNAME[NDESIGN]  = { 'A', 'B', 'C' };
static const char *DTEXT[NDESIGN] = {
    "naive: record valid if first field is not erased",
    "CRC check, but reuses a torn slot for the next record",
    "CRC check, and skips torn slots"
};

static int fd = -1;
static uint8_t sec[FF_SECTOR_BYTES];

typedef struct {
    uint32_t n, next;
    uint32_t slot[SLOTS];
} recov_t;

typedef struct {
    int have, cls, phase;
    uint32_t cut, seed, acked, torn_off, torn_kind;
    const char *why;
    uint8_t expected[SLOT], actual[SLOT];
} fail_t;

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

/* gen 0 = records written before the crash, gen 1 = records written after recovery (different data). */
static void make_rec(uint32_t j, uint32_t gen, uint8_t *o)
{
    uint32_t seq = j + 1u, c;
    uint32_t i;
    o[0] = (uint8_t)seq; o[1] = (uint8_t)(seq >> 8); o[2] = (uint8_t)(seq >> 16); o[3] = (uint8_t)(seq >> 24);
    for (i = 0; i < 24; i++) o[4 + i] = (uint8_t)(j * 31u + i * 7u + 0x5Au + gen * 0x6Bu);
    c = crc32_buf(o, 28);
    o[28] = (uint8_t)c; o[29] = (uint8_t)(c >> 8); o[30] = (uint8_t)(c >> 16); o[31] = (uint8_t)(c >> 24);
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static int slot_empty(const uint8_t *s) { uint32_t i; for (i = 0; i < SLOT; i++) if (s[i] != 0xFF) return 0; return 1; }
static int slot_crc_ok(const uint8_t *s) { return crc32_buf(s, 28) == le32(s + 28); }

static int load_sector(void)
{
    if (lseek(fd, 0, SEEK_SET) < 0) return -1;
    return read(fd, sec, sizeof(sec)) == (int)sizeof(sec) ? 0 : -1;
}

static void recover(int d, recov_t *r)
{
    uint32_t i;
    r->n = 0; r->next = 0;
    for (i = 0; i < SLOTS; i++) {
        const uint8_t *s = sec + i * SLOT;
        if (d == D_NAIVE) {
            if (le32(s) == 0xFFFFFFFFu) { r->next = i; return; }
            r->slot[r->n++] = i; r->next = i + 1;
        } else {
            if (slot_empty(s)) { r->next = i; return; }
            if (slot_crc_ok(s)) { r->slot[r->n++] = i; r->next = i + 1; }
            else if (d == D_CRC_REUSE) { r->next = i; return; }
            else { r->next = i + 1; }
        }
    }
}

static int append_rec(uint32_t slot, uint32_t j, uint32_t gen)
{
    uint8_t rec[SLOT];
    if (slot >= SLOTS) return -1;
    make_rec(j, gen, rec);
    if (lseek(fd, (off_t)slot * SLOT, SEEK_SET) < 0) return -1;
    return write(fd, rec, SLOT) == (int)SLOT ? 0 : -1;
}

static int verify(const recov_t *r, uint32_t min_n, uint32_t max_n, uint32_t gen_from, const char **why)
{
    uint8_t exp[SLOT];
    uint32_t t;
    for (t = 0; t < r->n; t++) {
        make_rec(t, t >= gen_from ? 1u : 0u, exp);
        if (memcmp(sec + r->slot[t] * SLOT, exp, SLOT) != 0) {
            *why = "a recovered record does not match what was written";
            return RES_CORRUPT;
        }
    }
    if (r->n < min_n) { *why = "acknowledged records are missing after recovery"; return RES_LOST; }
    if (r->n > max_n) { *why = "an extra record appeared"; return RES_CORRUPT; }
    return RES_OK;
}

static int dev_none(int cmd) { return devctl(fd, cmd, NULL, 0, NULL); }

static int run_once(int d, uint32_t cut, uint32_t seed, fail_t *f)
{
    recov_t r, r2;
    ff_powercut_t pc;
    ff_stats_t st;
    uint32_t j, next = 0, acked = 0, n, p;
    int res, phase = 1;
    const char *why = "";

    dev_none(FF_DCMD_RESET_ALL);
    if (cut) {
        pc.after_ops = cut - 1u; pc.seed = seed;
        devctl(fd, FF_DCMD_POWER_CUT, &pc, sizeof(pc), NULL);
    }
    for (j = 0; j < NREC; j++) {
        if (append_rec(next, j, 0u) != 0) break;
        next++; acked++;
    }
    memset(&st, 0, sizeof(st));
    devctl(fd, FF_DCMD_STATS, &st, sizeof(st), NULL);
    dev_none(FF_DCMD_POWER_RESTORE);

    if (load_sector() != 0) { why = "could not read the flash after recovery"; res = RES_LOST; goto done; }
    recover(d, &r);
    res = verify(&r, acked, acked + 1u, 0xFFFFFFFFu, &why);
    if (res != RES_OK) goto done;

    n = r.n;
    phase = 2;
    for (p = 0; p < NPOST; p++) {
        if (append_rec(r.next + p, n + p, 1u) != 0) { why = "append failed after recovery"; res = RES_LOST; goto done; }
    }
    load_sector();
    recover(d, &r2);
    res = verify(&r2, n + NPOST, n + NPOST, n, &why);

done:
    if (res != RES_OK && !f->have) {
        uint8_t exp[SLOT];
        f->have = 1; f->cls = res; f->phase = phase; f->cut = cut; f->seed = seed; f->acked = acked;
        f->why = why; f->torn_off = st.last_op_offset; f->torn_kind = st.last_op_kind;
        load_sector();
        make_rec(acked, phase == 2 ? 1u : 0u, exp);
        memcpy(f->expected, exp, SLOT);
        if (acked < SLOTS) memcpy(f->actual, sec + acked * SLOT, SLOT); else memset(f->actual, 0, SLOT);
    }
    return res;
}

int main(void)
{
    static fail_t fails[NDESIGN];
    uint32_t tot_ok[NDESIGN], tot_lost[NDESIGN], tot_cor[NDESIGN], tot_runs[NDESIGN];
    int d;
    uint32_t cut, seed, i;

    fd = open(FF_DEV_PATH, O_RDWR);
    if (fd < 0) { printf("FFCAMPRESULT|FAIL\n"); return 2; }

    printf("FFCAMPCFG|%u|%u|%u|%u\n", NREC, NPOST, SEEDS, SLOT);
    for (d = 0; d < NDESIGN; d++) {
        memset(&fails[d], 0, sizeof(fails[d]));
        tot_ok[d] = tot_lost[d] = tot_cor[d] = tot_runs[d] = 0;
        printf("FFCAMPDESIGN|%c|%s\n", DNAME[d], DTEXT[d]);
    }

    for (d = 0; d < NDESIGN; d++) {
        for (cut = 0; cut <= NREC; cut++) {
            char row[SEEDS + 1];
            uint32_t runs = cut ? SEEDS : 1u, ok = 0, lost = 0, cor = 0;
            for (seed = 1; seed <= runs; seed++) {
                int res = run_once(d, cut, seed, &fails[d]);
                row[seed - 1] = (char)('0' + res);
                if (res == RES_OK) ok++; else if (res == RES_LOST) lost++; else cor++;
            }
            row[runs] = '\0';
            printf("FFCAMPROW|%c|%u|%s\n", DNAME[d], cut, row);
            tot_runs[d] += runs; tot_ok[d] += ok; tot_lost[d] += lost; tot_cor[d] += cor;
        }
        printf("FFCAMPSUM|%c|%u|%u|%u|%u\n", DNAME[d], tot_runs[d], tot_ok[d], tot_lost[d], tot_cor[d]);
        if (fails[d].have) {
            fail_t *f = &fails[d];
            printf("FFCAMPFAIL|%c|%u|%u|%s|%d|%u|%u|%u|%s\n", DNAME[d], f->cut, f->seed,
                   f->cls == RES_LOST ? "LOST_ACKED" : "CORRUPT_ACCEPTED", f->phase, f->acked,
                   f->torn_kind, f->torn_off, f->why);
            printf("FFCAMPTORN|%c|", DNAME[d]);
            for (i = 0; i < SLOT; i++) printf("%02X", (unsigned)f->expected[i]);
            printf("|");
            for (i = 0; i < SLOT; i++) printf("%02X", (unsigned)f->actual[i]);
            printf("\n");
        }
    }

    dev_none(FF_DCMD_RESET_ALL);
    close(fd);
    /* The campaign must find the bugs in A and B, and find nothing wrong with C. */
    printf("FFCAMPRESULT|%s\n",
           (tot_lost[2] + tot_cor[2] == 0 && tot_lost[0] + tot_cor[0] > 0 && tot_lost[1] + tot_cor[1] > 0) ? "PASS" : "FAIL");
    return 0;
}
