/* flashforged - fault-injectable virtual SPI NOR flash resource manager for QNX 8.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <pthread.h>
#include <unistd.h>
#include <devctl.h>
#include "ff_ioctl.h"
#include "ff_chip.h"

/* one reply buffer per server thread: the reply is sent by the same thread after the handler returns */
static __thread uint8_t rbuf[16384];

#define FF_SERVER_THREADS 6

static void *server_thread(void *arg)
{
    dispatch_t *dpp = (dispatch_t *)arg;
    dispatch_context_t *ctx = dispatch_context_alloc(dpp);
    for (;;) {
        ctx = dispatch_block(ctx);
        if (ctx == NULL) { perror("dispatch_block"); return NULL; }
        dispatch_handler(ctx);
    }
}

static resmgr_connect_funcs_t connect_funcs;
static resmgr_io_funcs_t      io_funcs;
static iofunc_attr_t          attr;

static int io_read(resmgr_context_t *ctp, io_read_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_read_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    off_t off = ocb->offset;
    size_t n = msg->i.nbytes;
    if (off >= (off_t)FF_FLASH_BYTES) n = 0;
    else if ((size_t)off + n > FF_FLASH_BYTES) n = FF_FLASH_BYTES - (size_t)off;
    if (n > sizeof(rbuf)) n = sizeof(rbuf);

    if (n == 0) {
        _IO_SET_READ_NBYTES(ctp, 0);
        return _RESMGR_NPARTS(0);
    }
    /* The framework holds this resource's attribute lock while a handler runs, which would let only one
     * request run at a time. The chip has its own lock, so release the attribute lock around the part
     * that can block, and take it again before returning (allowed by the QNX docs). */
    iofunc_attr_unlock(ocb->attr);
    int rc = ff_chip_read((uint32_t)off, rbuf, (uint32_t)n);
    iofunc_attr_lock(ocb->attr);
    if (rc != 0) return rc;

    _IO_SET_READ_NBYTES(ctp, n);
    SETIOV(ctp->iov, rbuf, n);
    ocb->offset += n;
    ocb->attr->flags |= IOFUNC_ATTR_ATIME;
    return _RESMGR_NPARTS(1);
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_write_verify(ctp, msg, ocb, NULL);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;

    off_t off = ocb->offset;
    size_t n = msg->i.nbytes;
    if (off >= (off_t)FF_FLASH_BYTES) return ENOSPC;
    if ((size_t)off + n > FF_FLASH_BYTES) n = FF_FLASH_BYTES - (size_t)off;

    uint8_t buf[512];
    size_t done = 0;
    while (done < n) {
        size_t c = n - done;
        if (c > sizeof(buf)) c = sizeof(buf);
        ssize_t got = resmgr_msgread(ctp, buf, c, sizeof(msg->i) + done);
        if (got != (ssize_t)c) return EIO;
        int rc = ff_chip_program((uint32_t)off + (uint32_t)done, buf, (uint32_t)c);
        if (rc != 0) return rc;
        done += c;
    }

    _IO_SET_WRITE_NBYTES(ctp, n);
    ocb->offset += n;
    if (n) ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return _RESMGR_NPARTS(0);
}

static int reply_none(resmgr_context_t *ctp, io_devctl_t *msg)
{
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
}

static int reply_data(resmgr_context_t *ctp, io_devctl_t *msg, size_t n)
{
    memset(&msg->o, 0, sizeof(msg->o));
    msg->o.ret_val = 0;
    msg->o.nbytes = (int32_t)n;
    return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + n);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    void *data = _DEVCTL_DATA(msg->i);
    int rc;

    switch (msg->i.dcmd) {
    case FF_DCMD_GEOMETRY: {
        ff_geometry_t g;
        g.total_bytes  = FF_FLASH_BYTES;
        g.sector_bytes = FF_SECTOR_BYTES;
        g.page_bytes   = FF_PAGE_BYTES;
        g.sector_count = FF_SECTORS;
        memcpy(data, &g, sizeof(g));
        return reply_data(ctp, msg, sizeof(g));
    }
    case FF_DCMD_ERASE: {
        ff_erase_t er;
        memcpy(&er, data, sizeof(er));
        iofunc_attr_unlock(ocb->attr);      /* an erase can take many milliseconds: let other requests run */
        rc = ff_chip_erase(er.sector);
        iofunc_attr_lock(ocb->attr);
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_STATS: {
        ff_stats_t st;
        ff_chip_stats(&st);
        memcpy(data, &st, sizeof(st));
        return reply_data(ctp, msg, sizeof(st));
    }
    case FF_DCMD_RESET_ALL:
        ff_chip_reset();
        return reply_none(ctp, msg);
    case FF_DCMD_FAULT_CLEAR:
        ff_chip_lock();
        ff_core_fault_clear(&g_core);
        ff_chip_unlock();
        return reply_none(ctp, msg);
    case FF_DCMD_POWER_CUT: {
        ff_powercut_t pc;
        memcpy(&pc, data, sizeof(pc));
        ff_chip_lock();
        ff_core_power_cut(&g_core, pc.after_ops, pc.seed);
        ff_chip_unlock();
        return reply_none(ctp, msg);
    }
    case FF_DCMD_POWER_RESTORE:
        ff_chip_lock();
        ff_core_power_restore(&g_core);
        ff_chip_unlock();
        return reply_none(ctp, msg);
    case FF_DCMD_STUCK_BIT: {
        ff_stuck_t sb;
        memcpy(&sb, data, sizeof(sb));
        ff_chip_lock();
        rc = ff_core_stuck(&g_core, sb.offset, sb.bit, sb.value);
        ff_chip_unlock();
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_FLIP_BIT: {
        ff_flip_t fl;
        memcpy(&fl, data, sizeof(fl));
        ff_chip_lock();
        rc = ff_core_flip(&g_core, fl.offset, fl.bit);
        ff_chip_unlock();
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_WEAR_LIMIT: {
        ff_wear_t w;
        memcpy(&w, data, sizeof(w));
        ff_chip_lock();
        g_core.wear_limit = w.limit;
        ff_chip_unlock();
        return reply_none(ctp, msg);
    }
    case FF_DCMD_TIMING: {
        ff_timing_t t;
        memcpy(&t, data, sizeof(t));
        ff_chip_set_timing(&t);
        return reply_none(ctp, msg);
    }
    default:
        return ENOSYS;
    }
}

int main(void)
{
    ff_chip_init();

    dispatch_t *dpp = dispatch_create();
    if (dpp == NULL) { perror("dispatch_create"); return 1; }

    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof(rattr));
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 4096;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs,
                     _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read   = io_read;
    io_funcs.write  = io_write;
    io_funcs.devctl = io_devctl;

    iofunc_attr_init(&attr, S_IFCHR | 0666, NULL, NULL);
    attr.nbytes = FF_FLASH_BYTES;

    if (resmgr_attach(dpp, &rattr, FF_DEV_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach");
        return 1;
    }

    printf("flashforged: %s ready (%u KiB, %u-byte sectors, fault engine on, multithreaded)\n",
           FF_DEV_PATH, FF_FLASH_BYTES / 1024u, FF_SECTOR_BYTES);
    fflush(stdout);

    /* Several server threads, each waiting for messages on the same channel. QNX runs each handler at
     * the priority of the client that sent the message, so a high-priority reader is served at high
     * priority even while a low-priority client is in the middle of a slow erase. */
    {
        int i;
        pthread_t tid;
        for (i = 0; i < FF_SERVER_THREADS; i++) {
            if (pthread_create(&tid, NULL, server_thread, dpp) != 0) { perror("pthread_create"); return 1; }
        }
    }
    for (;;) pause();   /* the main thread has nothing left to do */
}
