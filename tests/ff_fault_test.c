/* ff_fault_test - proves each fault-injection command works.
 * Prints machine-readable lines: FFFT|name|PASS or FAIL, and FFFTRESULT|PASS/FAIL. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include "ff_ioctl.h"

static int fd = -1;
static int fails = 0;

static void check(const char *name, int ok)
{
    printf("FFFT|%s|%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static int none(int cmd) { return devctl(fd, cmd, NULL, 0, NULL); }
static int erase_sector(uint32_t s) { ff_erase_t e; e.sector = s; return devctl(fd, FF_DCMD_ERASE, &e, sizeof(e), NULL); }
static int arm_cut(uint32_t after, uint32_t seed) { ff_powercut_t p; p.after_ops = after; p.seed = seed; return devctl(fd, FF_DCMD_POWER_CUT, &p, sizeof(p), NULL); }
static int get_stats(ff_stats_t *s) { return devctl(fd, FF_DCMD_STATS, s, sizeof(*s), NULL); }
static int rd(off_t off, void *b, size_t n) { if (lseek(fd, off, SEEK_SET) < 0) return -1; return (int)read(fd, b, n); }
static int wr(off_t off, const void *b, size_t n) { if (lseek(fd, off, SEEK_SET) < 0) return -1; return (int)write(fd, b, n); }

static int all_ff(const uint8_t *b, size_t n) { size_t i; for (i = 0; i < n; i++) if (b[i] != 0xFF) return 0; return 1; }

/* Tear a 64-byte program with the given seed and read back what was left behind. */
static void torn_image(uint32_t seed, uint8_t *out)
{
    uint8_t zeros[64];
    memset(zeros, 0x00, sizeof(zeros));
    none(FF_DCMD_RESET_ALL);
    arm_cut(0, seed);
    wr(0, zeros, sizeof(zeros));
    none(FF_DCMD_POWER_RESTORE);
    rd(0, out, 64);
}

int main(void)
{
    ff_stats_t st;
    uint8_t a[64], b[64], buf[300], one;
    uint8_t pat[16];
    uint32_t seed;
    int i, distinct, ok;
    ff_stuck_t sb;
    ff_flip_t fl;
    ff_wear_t w;

    fd = open(FF_DEV_PATH, O_RDWR);
    if (fd < 0) { printf("FFFT|open_device|FAIL\n"); printf("FFFTRESULT|FAIL\n"); return 2; }

    ok = (none(FF_DCMD_RESET_ALL) == 0) && (rd(0, a, 16) == 16) && all_ff(a, 16);
    memset(&st, 1, sizeof(st));
    ok = ok && (get_stats(&st) == 0) && st.op_count == 0;
    check("reset_all_erases_and_zeroes_counters", ok);

    none(FF_DCMD_RESET_ALL);
    memset(buf, 0x11, sizeof(buf));
    erase_sector(0);
    wr(0, buf, 300);
    ok = (get_stats(&st) == 0) && st.erases == 1 && st.programs == 2 && st.op_count == 3;
    check("stats_count_erase_and_page_programs", ok);

    none(FF_DCMD_RESET_ALL);
    memset(pat, 0x3C, sizeof(pat));
    arm_cut(1, 7);
    ok = (wr(0, pat, 16) == 16);
    check("op_before_cut_completes", ok);
    ok = (wr(256, pat, 16) < 0);
    check("cut_op_fails", ok);
    ok = (get_stats(&st) == 0) && st.power_lost == 1 && st.torn_ops == 1 && st.last_op_kind == FF_OP_PROGRAM;
    check("stats_report_torn_program", ok);
    check("writes_fail_while_power_is_off", wr(512, pat, 16) < 0);
    check("reads_fail_while_power_is_off", rd(0, a, 4) < 0);
    ok = (none(FF_DCMD_POWER_RESTORE) == 0) && (rd(0, a, 16) == 16) && memcmp(a, pat, 16) == 0;
    check("power_restore_keeps_completed_data", ok);

    torn_image(99, a);
    torn_image(99, b);
    check("tear_is_deterministic_for_a_seed", memcmp(a, b, 64) == 0);

    distinct = 0;
    torn_image(1, a);
    for (seed = 2; seed <= 8; seed++) {
        torn_image(seed, b);
        if (memcmp(a, b, 64) != 0) distinct++;
    }
    check("tear_pattern_varies_with_seed", distinct >= 2);

    none(FF_DCMD_RESET_ALL);
    memset(buf, 0x00, 64);
    wr(5 * 4096, buf, 64);
    arm_cut(0, 3);
    ok = (erase_sector(5) != 0);
    ok = ok && (get_stats(&st) == 0) && st.torn_ops == 1 && st.last_op_kind == FF_OP_ERASE;
    none(FF_DCMD_POWER_RESTORE);
    check("torn_erase_is_reported", ok);

    none(FF_DCMD_RESET_ALL);
    sb.offset = 100; sb.bit = 3; sb.value = 0;
    ok = (devctl(fd, FF_DCMD_STUCK_BIT, &sb, sizeof(sb), NULL) == 0) && (rd(100, &one, 1) == 1) && one == 0xF7;
    none(FF_DCMD_FAULT_CLEAR);
    ok = ok && (rd(100, &one, 1) == 1) && one == 0xFF;
    check("stuck_at_0_bit_and_clear", ok);

    none(FF_DCMD_RESET_ALL);
    one = 0x00;
    wr(200, &one, 1);
    sb.offset = 200; sb.bit = 0; sb.value = 1;
    ok = (devctl(fd, FF_DCMD_STUCK_BIT, &sb, sizeof(sb), NULL) == 0) && (rd(200, &one, 1) == 1) && one == 0x01;
    check("stuck_at_1_bit", ok);

    none(FF_DCMD_RESET_ALL);
    fl.offset = 300; fl.bit = 5;
    ok = (devctl(fd, FF_DCMD_FLIP_BIT, &fl, sizeof(fl), NULL) == 0) && (rd(300, &one, 1) == 1) && one == 0xDF;
    check("flip_bit_models_bit_rot", ok);

    none(FF_DCMD_RESET_ALL);
    w.limit = 3;
    devctl(fd, FF_DCMD_WEAR_LIMIT, &w, sizeof(w), NULL);
    ok = 1;
    for (i = 0; i < 3; i++) if (erase_sector(7) != 0) ok = 0;
    ok = ok && (erase_sector(7) != 0);
    ok = ok && (get_stats(&st) == 0) && st.max_erase_count == 3;
    check("wear_limit_blocks_erase", ok);

    sb.offset = 100; sb.bit = 3; sb.value = 0;
    devctl(fd, FF_DCMD_STUCK_BIT, &sb, sizeof(sb), NULL);
    ok = (none(FF_DCMD_RESET_ALL) == 0) && (erase_sector(7) == 0) && (rd(100, &one, 1) == 1) && one == 0xFF;
    check("reset_clears_all_faults", ok);

    none(FF_DCMD_RESET_ALL);
    close(fd);
    printf("FFFTRESULT|%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
