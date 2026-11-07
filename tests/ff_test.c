/* ff_test - verifies NOR semantics of /dev/spinor0.
 * Prints machine-readable lines (FFTEST|FFBYTES|FFMETRIC|FFGEOM|FFRESULT)
 * that the Kaggle notebook parses into tables and charts. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include "ff_ioctl.h"

static int fails = 0;
static int fd = -1;

static void check(const char *name, int ok)
{
    printf("FFTEST|%s|%s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static double now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

static int erase_sector(uint32_t s)
{
    ff_erase_t er;
    er.sector = s;
    return devctl(fd, FF_DCMD_ERASE, &er, sizeof(er), NULL);
}

static int rd(off_t off, void *buf, size_t n)
{
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return (int)read(fd, buf, n);
}

static int wr(off_t off, const void *buf, size_t n)
{
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    return (int)write(fd, buf, n);
}

static void dump(const char *label, off_t off, size_t n)
{
    uint8_t b[16];
    size_t i;
    if (n > sizeof(b)) n = sizeof(b);
    memset(b, 0, sizeof(b));
    rd(off, b, n);
    printf("FFBYTES|%s|", label);
    for (i = 0; i < n; i++) printf("%02X", (unsigned)b[i]);
    printf("\n");
}

int main(void)
{
    ff_geometry_t g;
    uint8_t buf[4096];
    uint8_t chk[16];
    uint8_t p1[4] = { 0xF0, 0xFF, 0xAA, 0x55 };
    uint8_t p2[4] = { 0x3C, 0x0F, 0xFF, 0xF0 };
    uint8_t p3[4] = { 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t expect[4] = { 0x30, 0x0F, 0xAA, 0x50 };
    int i, ok;
    double t0;

    fd = open(FF_DEV_PATH, O_RDWR);
    if (fd < 0) {
        printf("FFTEST|open_device|FAIL\n");
        printf("FFRESULT|FAIL\n");
        return 2;
    }
    check("open_device", 1);

    memset(&g, 0, sizeof(g));
    check("geometry_devctl", devctl(fd, FF_DCMD_GEOMETRY, &g, sizeof(g), NULL) == 0);
    printf("FFGEOM|%u|%u|%u|%u\n", (unsigned)g.total_bytes, (unsigned)g.sector_bytes,
           (unsigned)g.page_bytes, (unsigned)g.sector_count);
    check("geometry_values", g.total_bytes == 1048576u && g.sector_bytes == 4096u &&
                             g.page_bytes == 256u && g.sector_count == 256u);

    check("erase_sector0", erase_sector(0) == 0);
    dump("erased", 0, 4);
    memset(chk, 0, sizeof(chk));
    ok = (rd(0, chk, 16) == 16);
    for (i = 0; i < 16; i++) if (chk[i] != 0xFF) ok = 0;
    check("erased_reads_FF", ok);

    check("program_first", wr(0, p1, 4) == 4);
    dump("program1", 0, 4);
    check("program_second", wr(0, p2, 4) == 4);
    dump("program2", 0, 4);
    memset(chk, 0, sizeof(chk));
    ok = (rd(0, chk, 4) == 4) && memcmp(chk, expect, 4) == 0;
    check("nor_and_semantics", ok);

    ok = (wr(0, p3, 4) == 4);
    dump("program_FF", 0, 4);
    memset(chk, 0, sizeof(chk));
    ok = ok && (rd(0, chk, 4) == 4) && memcmp(chk, expect, 4) == 0;
    check("ones_do_not_set_bits", ok);

    check("erase_restores_FF", erase_sector(0) == 0);
    dump("erased_again", 0, 4);
    memset(chk, 0, sizeof(chk));
    ok = (rd(0, chk, 4) == 4) && memcmp(chk, p3, 4) == 0;
    check("erase_readback_FF", ok);

    erase_sector(0);
    erase_sector(1);
    wr(4096, p1, 4);
    erase_sector(0);
    memset(chk, 0, sizeof(chk));
    ok = (rd(4096, chk, 4) == 4) && memcmp(chk, p1, 4) == 0;
    check("sector_isolation", ok);
    erase_sector(1);

    check("erase_out_of_range_rejected", erase_sector(256) != 0);
    lseek(fd, 1048576, SEEK_SET);
    check("write_past_end_rejected", write(fd, p1, 1) < 0);
    memset(chk, 0, sizeof(chk));
    check("read_clipped_at_end", rd(1048576 - 2, chk, 16) == 2);

    for (i = 0; i < 20; i++) {
        t0 = now_us();
        erase_sector(2);
        printf("FFMETRIC|erase_sector_us|%.1f\n", now_us() - t0);
    }
    memset(buf, 0x5A, 256);
    erase_sector(3);
    for (i = 0; i < 16; i++) {
        t0 = now_us();
        wr(3 * 4096 + i * 256, buf, 256);
        printf("FFMETRIC|program_page_us|%.1f\n", now_us() - t0);
    }
    for (i = 0; i < 20; i++) {
        t0 = now_us();
        rd(3 * 4096, buf, 4096);
        printf("FFMETRIC|read_sector_us|%.1f\n", now_us() - t0);
    }
    erase_sector(3);

    close(fd);
    printf("FFRESULT|%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
