/* ff_core.h - NOR flash model and fault engine.
 * Pure C with no QNX dependencies, so the same code runs inside the QNX
 * resource manager and in host-side tests. */
#ifndef FF_CORE_H
#define FF_CORE_H

#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "ff_ioctl.h"

#define FF_MAX_STUCK 16

typedef struct {
    uint32_t offset;
    uint8_t  bit;
    uint8_t  value;
} ff_stuck_cell_t;

typedef struct {
    uint8_t         mem[FF_FLASH_BYTES];
    uint32_t        erase_count[FF_SECTORS];
    uint32_t        wear_limit;      /* 0 = unlimited */
    uint64_t        op_count;        /* program (per page) + erase operations since reset */
    uint64_t        reads, programs, erases, torn_ops;
    int             cut_armed;
    uint32_t        cut_remaining;   /* operations still allowed to finish before the tear */
    uint32_t        cut_seed;
    int             power_lost;
    uint32_t        stuck_n;
    ff_stuck_cell_t stuck[FF_MAX_STUCK];
    uint32_t        last_kind, last_off, last_len;
} ff_core_t;

static uint32_t ff_rng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x;
    return x;
}

static void ff_core_reset(ff_core_t *c)
{
    memset(c->mem, 0xFF, sizeof(c->mem));
    memset(c->erase_count, 0, sizeof(c->erase_count));
    c->wear_limit = 0;
    c->op_count = c->reads = c->programs = c->erases = c->torn_ops = 0;
    c->cut_armed = 0; c->cut_remaining = 0; c->cut_seed = 0;
    c->power_lost = 0;
    c->stuck_n = 0;
    c->last_kind = FF_OP_NONE; c->last_off = 0; c->last_len = 0;
}

static void ff_core_fault_clear(ff_core_t *c)
{
    c->wear_limit = 0;
    c->cut_armed = 0;
    c->stuck_n = 0;
}

static void ff_core_power_cut(ff_core_t *c, uint32_t after_ops, uint32_t seed)
{
    c->cut_armed = 1;
    c->cut_remaining = after_ops;
    c->cut_seed = seed;
}

static void ff_core_power_restore(ff_core_t *c)
{
    c->power_lost = 0;
    c->cut_armed = 0;
}

static int ff_core_stuck(ff_core_t *c, uint32_t off, uint32_t bit, uint32_t value)
{
    if (off >= FF_FLASH_BYTES || bit > 7) return EINVAL;
    if (c->stuck_n >= FF_MAX_STUCK) return ENOSPC;
    c->stuck[c->stuck_n].offset = off;
    c->stuck[c->stuck_n].bit = (uint8_t)bit;
    c->stuck[c->stuck_n].value = (uint8_t)(value ? 1 : 0);
    c->stuck_n++;
    return 0;
}

static int ff_core_flip(ff_core_t *c, uint32_t off, uint32_t bit)
{
    if (off >= FF_FLASH_BYTES || bit > 7) return EINVAL;
    c->mem[off] ^= (uint8_t)(1u << bit);
    return 0;
}

/* Returns 0 and sets *tear when this operation must be cut short; EIO when the power is already off. */
static int ff_core_begin_op(ff_core_t *c, uint32_t kind, uint32_t off, uint32_t len, int *tear)
{
    *tear = 0;
    if (c->power_lost) return EIO;
    c->op_count++;
    c->last_kind = kind; c->last_off = off; c->last_len = len;
    if (c->cut_armed) {
        if (c->cut_remaining == 0) {
            *tear = 1;
            c->torn_ops++;
            c->power_lost = 1;
            c->cut_armed = 0;
        } else {
            c->cut_remaining--;
        }
    }
    return 0;
}

static uint32_t ff_tear_seed(const ff_core_t *c)
{
    uint32_t s = c->cut_seed * 2654435761u ^ (uint32_t)c->op_count * 40503u ^ 0x9E3779B9u;
    return s ? s : 1u;
}

/* Power failed during a page program: a prefix was programmed, one byte only partly, the rest untouched. */
static void ff_tear_program(ff_core_t *c, uint32_t off, const uint8_t *d, uint32_t n)
{
    uint32_t s = ff_tear_seed(c);
    uint32_t k = ff_rng(&s) % n;
    uint8_t rnd = (uint8_t)ff_rng(&s);
    uint32_t i;
    for (i = 0; i < k; i++) c->mem[off + i] &= d[i];
    c->mem[off + k] &= (uint8_t)(d[k] | rnd);
}

/* Power failed during a sector erase: a prefix is erased, one byte only partly, the rest keeps old data. */
static void ff_tear_erase(ff_core_t *c, uint32_t sector)
{
    uint32_t s = ff_tear_seed(c);
    uint32_t base = sector * FF_SECTOR_BYTES;
    uint32_t k = ff_rng(&s) % FF_SECTOR_BYTES;
    uint8_t rnd = (uint8_t)ff_rng(&s);
    memset(c->mem + base, 0xFF, k);
    c->mem[base + k] |= rnd;
}

static int ff_core_erase(ff_core_t *c, uint32_t sector)
{
    int tear, rc;
    if (sector >= FF_SECTORS) return EINVAL;
    rc = ff_core_begin_op(c, FF_OP_ERASE, sector * FF_SECTOR_BYTES, FF_SECTOR_BYTES, &tear);
    if (rc) return rc;
    if (tear) { ff_tear_erase(c, sector); return EIO; }
    if (c->wear_limit && c->erase_count[sector] >= c->wear_limit) return EIO; /* worn out: erase has no effect */
    memset(c->mem + (size_t)sector * FF_SECTOR_BYTES, 0xFF, FF_SECTOR_BYTES);
    c->erase_count[sector]++;
    c->erases++;
    return 0;
}

/* Programs n bytes; the span is split at page boundaries and every page is one operation. */
static int ff_core_program(ff_core_t *c, uint32_t off, const uint8_t *data, uint32_t n)
{
    uint32_t done = 0;
    while (done < n) {
        uint32_t pos = off + done;
        uint32_t page_end = (pos / FF_PAGE_BYTES + 1) * FF_PAGE_BYTES;
        uint32_t chunk = n - done;
        uint32_t i;
        int tear, rc;
        if (chunk > page_end - pos) chunk = page_end - pos;
        rc = ff_core_begin_op(c, FF_OP_PROGRAM, pos, chunk, &tear);
        if (rc) return rc;
        if (tear) { ff_tear_program(c, pos, data + done, chunk); return EIO; }
        for (i = 0; i < chunk; i++) c->mem[pos + i] &= data[done + i]; /* NOR: program only clears bits */
        c->programs++;
        done += chunk;
    }
    return 0;
}

static int ff_core_read(ff_core_t *c, uint32_t off, uint8_t *out, uint32_t n)
{
    uint32_t i;
    if (c->power_lost) return EIO;
    memcpy(out, c->mem + off, n);
    for (i = 0; i < c->stuck_n; i++) {
        uint32_t o = c->stuck[i].offset;
        if (o >= off && o < off + n) {
            if (c->stuck[i].value) out[o - off] |= (uint8_t)(1u << c->stuck[i].bit);
            else                   out[o - off] &= (uint8_t)~(1u << c->stuck[i].bit);
        }
    }
    c->reads++;
    return 0;
}

static void ff_core_stats(const ff_core_t *c, ff_stats_t *s)
{
    uint32_t i, m = 0;
    memset(s, 0, sizeof(*s));
    s->op_count = c->op_count; s->reads = c->reads; s->programs = c->programs;
    s->erases = c->erases; s->torn_ops = c->torn_ops;
    s->power_lost = (uint32_t)c->power_lost; s->wear_limit = c->wear_limit;
    s->stuck_count = c->stuck_n;
    for (i = 0; i < FF_SECTORS; i++) if (c->erase_count[i] > m) m = c->erase_count[i];
    s->max_erase_count = m;
    s->last_op_kind = c->last_kind; s->last_op_offset = c->last_off; s->last_op_len = c->last_len;
}

#endif
