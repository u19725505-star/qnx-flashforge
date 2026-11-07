/* flashforged - fault-injectable virtual SPI NOR flash resource manager for QNX 8.0 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <devctl.h>
#include "ff_ioctl.h"
#include "ff_core.h"

static ff_core_t core;
static uint8_t   rbuf[65536];

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
    int rc = ff_core_read(&core, (uint32_t)off, rbuf, (uint32_t)n);
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
        int rc = ff_core_program(&core, (uint32_t)off + (uint32_t)done, buf, (uint32_t)c);
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
        rc = ff_core_erase(&core, er.sector);
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_STATS: {
        ff_stats_t st;
        ff_core_stats(&core, &st);
        memcpy(data, &st, sizeof(st));
        return reply_data(ctp, msg, sizeof(st));
    }
    case FF_DCMD_RESET_ALL:
        ff_core_reset(&core);
        return reply_none(ctp, msg);
    case FF_DCMD_FAULT_CLEAR:
        ff_core_fault_clear(&core);
        return reply_none(ctp, msg);
    case FF_DCMD_POWER_CUT: {
        ff_powercut_t pc;
        memcpy(&pc, data, sizeof(pc));
        ff_core_power_cut(&core, pc.after_ops, pc.seed);
        return reply_none(ctp, msg);
    }
    case FF_DCMD_POWER_RESTORE:
        ff_core_power_restore(&core);
        return reply_none(ctp, msg);
    case FF_DCMD_STUCK_BIT: {
        ff_stuck_t sb;
        memcpy(&sb, data, sizeof(sb));
        rc = ff_core_stuck(&core, sb.offset, sb.bit, sb.value);
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_FLIP_BIT: {
        ff_flip_t fl;
        memcpy(&fl, data, sizeof(fl));
        rc = ff_core_flip(&core, fl.offset, fl.bit);
        if (rc != 0) return rc;
        return reply_none(ctp, msg);
    }
    case FF_DCMD_WEAR_LIMIT: {
        ff_wear_t w;
        memcpy(&w, data, sizeof(w));
        core.wear_limit = w.limit;
        return reply_none(ctp, msg);
    }
    default:
        return ENOSYS;
    }
}

int main(void)
{
    ff_core_reset(&core);

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

    printf("flashforged: %s ready (%u KiB, %u-byte sectors, fault engine on)\n",
           FF_DEV_PATH, FF_FLASH_BYTES / 1024u, FF_SECTOR_BYTES);

    dispatch_context_t *ctx = dispatch_context_alloc(dpp);
    for (;;) {
        ctx = dispatch_block(ctx);
        if (ctx == NULL) { perror("dispatch_block"); return 1; }
        dispatch_handler(ctx);
    }
}
