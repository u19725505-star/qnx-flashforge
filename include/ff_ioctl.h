#ifndef FF_IOCTL_H
#define FF_IOCTL_H

#include <stdint.h>
#include <devctl.h>

#ifndef _DCMD_MISC
#define _DCMD_MISC 0x0c
#endif

#define FF_DEV_PATH "/dev/spinor0"

typedef struct {
    uint32_t total_bytes;
    uint32_t sector_bytes;
    uint32_t page_bytes;
    uint32_t sector_count;
} ff_geometry_t;

typedef struct {
    uint32_t sector;
} ff_erase_t;

#define FF_DCMD_GEOMETRY __DIOF(_DCMD_MISC, 0x100, ff_geometry_t)
#define FF_DCMD_ERASE    __DIOT(_DCMD_MISC, 0x101, ff_erase_t)

#endif
