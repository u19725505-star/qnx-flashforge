/* ff_test - checks NOR semantics of /dev/spinor0 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <devctl.h>
#include "ff_ioctl.h"

static int fails = 0;

static void check(const char *name, int ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) fails++;
}

int main(void)
{
    int fd = open(FF_DEV_PATH, O_RDWR);
    if (fd < 0) { perror("open"); return 2; }

    ff_geometry_t g;
    check("geometry devctl", devctl(fd, FF_DCMD_GEOMETRY, &g, sizeof(g), NULL) == 0);
    printf("  total=%u sector=%u page=%u sectors=%u\n",
           g.total_bytes, g.sector_bytes, g.page_bytes, g.sector_count);

    ff_erase_t er = { 0 };
    check("erase sector 0", devctl(fd, FF_DCMD_ERASE, &er, sizeof(er), NULL) == 0);

    uint8_t b = 0;
    lseek(fd, 0, SEEK_SET);
    check("erased byte reads 0xFF", read(fd, &b, 1) == 1 && b == 0xFF);

    uint8_t w1 = 0xF0, w2 = 0x3C;
    lseek(fd, 0, SEEK_SET); write(fd, &w1, 1);
    lseek(fd, 0, SEEK_SET); write(fd, &w2, 1);
    lseek(fd, 0, SEEK_SET);
    check("second write ANDs bits (0xF0 & 0x3C == 0x30)", read(fd, &b, 1) == 1 && b == 0x30);

    devctl(fd, FF_DCMD_ERASE, &er, sizeof(er), NULL);
    lseek(fd, 0, SEEK_SET);
    check("erase restores 0xFF", read(fd, &b, 1) == 1 && b == 0xFF);

    close(fd);
    printf("%s\n", fails ? "RESULT: FAIL" : "RESULT: ALL PASS");
    return fails ? 1 : 0;
}
