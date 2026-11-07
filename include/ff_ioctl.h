#ifndef FF_IOCTL_H
#define FF_IOCTL_H

#include <stdint.h>
#include <devctl.h>

#ifndef _DCMD_MISC
#define _DCMD_MISC 0x0c
#endif

#define FF_DEV_PATH "/dev/spinor0"

#define FF_FLASH_BYTES  1048576u
#define FF_SECTOR_BYTES 4096u
#define FF_PAGE_BYTES   256u
#define FF_SECTORS      (FF_FLASH_BYTES / FF_SECTOR_BYTES)

#define FF_OP_NONE    0u
#define FF_OP_PROGRAM 1u
#define FF_OP_ERASE   2u

typedef struct {
    uint32_t total_bytes;
    uint32_t sector_bytes;
    uint32_t page_bytes;
    uint32_t sector_count;
} ff_geometry_t;

typedef struct { uint32_t sector; } ff_erase_t;

/* after_ops = number of program/erase operations allowed to finish; the next one is torn. */
typedef struct { uint32_t after_ops; uint32_t seed; } ff_powercut_t;
typedef struct { uint32_t offset; uint32_t bit; uint32_t value; } ff_stuck_t;
typedef struct { uint32_t offset; uint32_t bit; } ff_flip_t;
typedef struct { uint32_t limit; } ff_wear_t;

typedef struct {
    uint64_t op_count;
    uint64_t reads;
    uint64_t programs;
    uint64_t erases;
    uint64_t torn_ops;
    uint32_t power_lost;
    uint32_t wear_limit;
    uint32_t stuck_count;
    uint32_t max_erase_count;
    uint32_t last_op_kind;
    uint32_t last_op_offset;
    uint32_t last_op_len;
    uint32_t reserved;
} ff_stats_t;

#define FF_DCMD_GEOMETRY      __DIOF(_DCMD_MISC, 0x100, ff_geometry_t)
#define FF_DCMD_ERASE         __DIOT(_DCMD_MISC, 0x101, ff_erase_t)
#define FF_DCMD_STATS         __DIOF(_DCMD_MISC, 0x102, ff_stats_t)
#define FF_DCMD_RESET_ALL     __DION(_DCMD_MISC, 0x103)
#define FF_DCMD_FAULT_CLEAR   __DION(_DCMD_MISC, 0x104)
#define FF_DCMD_POWER_CUT     __DIOT(_DCMD_MISC, 0x105, ff_powercut_t)
#define FF_DCMD_POWER_RESTORE __DION(_DCMD_MISC, 0x106)
#define FF_DCMD_STUCK_BIT     __DIOT(_DCMD_MISC, 0x107, ff_stuck_t)
#define FF_DCMD_FLIP_BIT      __DIOT(_DCMD_MISC, 0x108, ff_flip_t)
#define FF_DCMD_WEAR_LIMIT    __DIOT(_DCMD_MISC, 0x109, ff_wear_t)

#endif
