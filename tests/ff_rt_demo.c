/* ff_rt_demo - real-time demo.
 * A high-priority task reads the flash every 5 ms and must finish before the next period (5 ms).
 * A low-priority task erases sectors back to back; one erase takes 45 ms on this simulated chip.
 *   idle      no erase running: the floor for read latency on this machine
 *   blocking  erase keeps the chip busy: reads must wait for it
 *   suspend   erase runs in 2 ms slices and steps aside whenever a read is waiting
 * Prints FFRT... lines that the notebook turns into tables and charts. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <devctl.h>
#include "ff_ioctl.h"

#define N_READS      400
#define PERIOD_US    5000
#define DEADLINE_US  5000
#define ERASE_US     45000
#define SLICE_US     2000
#define READER_PRIO  40
#define ERASER_PRIO  10

static volatile int stop_eraser;
static volatile int erases_done;
static int prio_ok = 1;
static uint32_t lat[N_READS];

static double ts_us(const struct timespec *t) { return (double)t->tv_sec * 1e6 + (double)t->tv_nsec / 1e3; }
static double now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return ts_us(&t); }

static void set_prio(int prio)
{
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = prio;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) prio_ok = 0;
}

static void *eraser(void *arg)
{
    int fd = open(FF_DEV_PATH, O_RDWR);
    ff_erase_t e;
    uint32_t i = 0;
    (void)arg;
    set_prio(ERASER_PRIO);
    while (!stop_eraser) {
        e.sector = 10u + (i++ % 4u);
        if (devctl(fd, FF_DCMD_ERASE, &e, sizeof(e), NULL) == 0) erases_done++;
    }
    close(fd);
    return NULL;
}

static void sort_u32(uint32_t *a, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        uint32_t v = a[i];
        for (j = i - 1; j >= 0 && a[j] > v; j--) a[j + 1] = a[j];
        a[j + 1] = v;
    }
}

static double run_scenario(const char *name, uint32_t mode, int with_eraser, uint32_t *p99_out, int *miss_out)
{
    ff_timing_t t;
    pthread_t th;
    struct timespec next;
    uint32_t sorted[N_READS];
    uint8_t buf[64];
    char line[N_READS * 8 + 64];
    int ctl = open(FF_DEV_PATH, O_RDWR);
    int fd, i, misses = 0, pos;
    double t0, elapsed_ms;

    t.mode = mode; t.erase_us = ERASE_US; t.slice_us = SLICE_US; t.reserved = 0;
    devctl(ctl, FF_DCMD_TIMING, &t, sizeof(t), NULL);
    erases_done = 0;
    stop_eraser = 0;
    if (with_eraser) pthread_create(&th, NULL, eraser, NULL);
    usleep(30000);

    fd = open(FF_DEV_PATH, O_RDWR);
    clock_gettime(CLOCK_MONOTONIC, &next);
    t0 = now_us();
    for (i = 0; i < N_READS; i++) {
        double release;
        next.tv_nsec += PERIOD_US * 1000L;
        while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        release = ts_us(&next);
        lseek(fd, 0, SEEK_SET);
        read(fd, buf, sizeof(buf));
        {
            double l = now_us() - release;
            lat[i] = l < 0 ? 0u : (uint32_t)l;
        }
        if (lat[i] > DEADLINE_US) misses++;
    }
    elapsed_ms = (now_us() - t0) / 1000.0;
    stop_eraser = 1;
    if (with_eraser) pthread_join(th, NULL);
    close(fd);

    memcpy(sorted, lat, sizeof(sorted));
    sort_u32(sorted, N_READS);
    printf("FFRTSUM|%s|%d|%u|%u|%u|%d|%d|%.0f\n", name, N_READS, sorted[N_READS / 2], sorted[(N_READS * 99) / 100],
           sorted[N_READS - 1], misses, erases_done, elapsed_ms);
    pos = snprintf(line, sizeof(line), "FFRTLAT|%s|", name);
    for (i = 0; i < N_READS; i++) pos += snprintf(line + pos, sizeof(line) - (size_t)pos, "%u%s", lat[i], i + 1 < N_READS ? "," : "");
    printf("%s\n", line);

    t.mode = FF_TIMING_INSTANT;
    devctl(ctl, FF_DCMD_TIMING, &t, sizeof(t), NULL);
    close(ctl);
    *p99_out = sorted[(N_READS * 99) / 100];
    *miss_out = misses;
    return elapsed_ms;
}

int main(void)
{
    uint32_t p99_idle, p99_block, p99_susp;
    int miss_idle, miss_block, miss_susp;

    set_prio(READER_PRIO);
    printf("FFRTCFG|%d|%d|%d|%d|%d|%d\n", PERIOD_US, DEADLINE_US, ERASE_US, SLICE_US, N_READS, prio_ok);
    run_scenario("idle", FF_TIMING_BLOCKING, 0, &p99_idle, &miss_idle);
    run_scenario("blocking", FF_TIMING_BLOCKING, 1, &p99_block, &miss_block);
    run_scenario("suspend", FF_TIMING_SUSPEND, 1, &p99_susp, &miss_susp);
    (void)p99_idle; (void)miss_idle;
    {
        ff_stats_t st;
        int ctl = open(FF_DEV_PATH, O_RDWR);
        memset(&st, 0, sizeof(st));
        devctl(ctl, FF_DCMD_STATS, &st, sizeof(st), NULL);
        printf("FFRTINFO|%u|%u\n", (unsigned)st.peak_handlers, (unsigned)st.distinct_threads);
        close(ctl);
    }
    /* The demo works if suspending the erase clearly beats blocking on tail latency and missed deadlines. */
    printf("FFRTRESULT|%s\n", (p99_susp * 3u < p99_block && miss_susp < miss_block) ? "PASS" : "FAIL");
    return 0;
}
