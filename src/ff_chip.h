/* ff_chip.h - thread-safe chip with simulated timing, on top of ff_core.h.
 * Portable POSIX threads code: it runs inside the QNX resource manager and in host tests.
 *
 * The chip is one shared resource guarded by a priority-inheriting mutex.
 *   mode 1 (blocking): an erase keeps the chip busy for erase_us; every read waits for it.
 *   mode 2 (suspend):  the erase runs in slices without holding the lock; a read arriving during it
 *                      pauses it for a short suspend latency and is served at once (NOR "erase suspend"). */
#ifndef FF_CHIP_H
#define FF_CHIP_H

#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "ff_core.h"

static ff_core_t        g_core;
static pthread_mutex_t  g_lock;
static ff_timing_t      g_timing;
static volatile int     g_pending_reads;
static volatile int     g_erasing;        /* suspend mode: an erase is running and the lock is NOT held */
static volatile int     g_handlers, g_peak;

#define FF_SUSPEND_LATENCY_US 100u       /* time a NOR chip needs to pause an erase for a read */

static pthread_mutex_t  g_tid_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long    g_tids[32];
static int              g_ntids;

static void ff_note_thread(void)
{
    unsigned long id = (unsigned long)pthread_self();
    int i;
    pthread_mutex_lock(&g_tid_lock);
    for (i = 0; i < g_ntids; i++) if (g_tids[i] == id) break;
    if (i == g_ntids && g_ntids < 32) g_tids[g_ntids++] = id;
    pthread_mutex_unlock(&g_tid_lock);
}

static void ff_enter(void)
{
    int n = __sync_add_and_fetch(&g_handlers, 1), p;
    ff_note_thread();
    while (n > (p = g_peak)) { if (__sync_bool_compare_and_swap(&g_peak, p, n)) break; }
}
static void ff_leave(void) { __sync_fetch_and_sub(&g_handlers, 1); }

static void ff_sleep_us(uint32_t us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}

static void ff_chip_init(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&g_lock, &a);
    ff_core_reset(&g_core);
    memset(&g_timing, 0, sizeof(g_timing));
    g_timing.erase_us = 45000u;
    g_timing.slice_us = 2000u;
}

static void ff_chip_lock(void)   { pthread_mutex_lock(&g_lock); }
static void ff_chip_unlock(void) { pthread_mutex_unlock(&g_lock); }

static void ff_chip_reset(void)
{
    ff_chip_lock();
    ff_core_reset(&g_core);
    g_timing.mode = FF_TIMING_INSTANT;
    ff_chip_unlock();
}

static void ff_chip_set_timing(const ff_timing_t *t)
{
    ff_chip_lock();
    g_timing = *t;
    if (g_timing.slice_us == 0) g_timing.slice_us = 1000u;
    ff_chip_unlock();
}

static int ff_chip_read(uint32_t off, uint8_t *out, uint32_t n)
{
    int rc;
    ff_enter();
    if (g_timing.mode == FF_TIMING_SUSPEND && g_erasing)
        ff_sleep_us(FF_SUSPEND_LATENCY_US);        /* the chip pauses the running erase, then serves the read */
    __sync_fetch_and_add(&g_pending_reads, 1);
    ff_chip_lock();
    rc = ff_core_read(&g_core, off, out, n);
    ff_chip_unlock();
    __sync_fetch_and_sub(&g_pending_reads, 1);
    ff_leave();
    return rc;
}

static int ff_chip_program(uint32_t off, const uint8_t *data, uint32_t n)
{
    int rc;
    ff_enter();
    ff_chip_lock();
    rc = ff_core_program(&g_core, off, data, n);
    ff_chip_unlock();
    ff_leave();
    return rc;
}

static int ff_chip_erase_inner(uint32_t sector)
{
    int rc;
    uint32_t remaining, slice;

    ff_chip_lock();
    rc = ff_core_erase(&g_core, sector);
    if (rc != 0 || g_timing.mode == FF_TIMING_INSTANT) {
        ff_chip_unlock();
        return rc;
    }
    remaining = g_timing.erase_us;
    if (g_timing.mode == FF_TIMING_BLOCKING) {
        ff_sleep_us(remaining);           /* chip busy: the lock is held, so every read waits */
        ff_chip_unlock();
        return 0;
    }
    /* suspend mode: the erase runs in slices WITHOUT holding the lock, so reads are never stuck behind it */
    g_erasing = 1;
    ff_chip_unlock();
    slice = g_timing.slice_us;
    while (remaining > 0) {
        uint32_t s = remaining < slice ? remaining : slice;
        int spins = 0;
        ff_sleep_us(s);
        remaining -= s;
        if (remaining == 0) break;
        while (g_pending_reads > 0 && spins++ < 100000) sched_yield();   /* let waiting reads finish first */
    }
    g_erasing = 0;
    return 0;
}

static int ff_chip_erase(uint32_t sector)
{
    int rc;
    ff_enter();
    rc = ff_chip_erase_inner(sector);
    ff_leave();
    return rc;
}

static void ff_chip_stats(ff_stats_t *s)
{
    ff_chip_lock();
    ff_core_stats(&g_core, s);
    ff_chip_unlock();
    s->peak_handlers = (uint32_t)g_peak;
    s->distinct_threads = (uint32_t)g_ntids;
}

#endif
