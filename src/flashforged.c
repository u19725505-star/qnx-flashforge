/* flashforged - virtual SPI NOR flash resource manager for QNX 8.0 */
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

#define FLASH_BYTES  (1024u * 1024u)
#define SECTOR_BYTES 4096u
#define PAGE_BYTES   256u

static uint8_t flash[FLASH_BYTES];

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
    if (off >= (off_t)FLASH_BYTES) n = 0;
    else if ((size_t)off + n > FLASH_BYTES) n = FLASH_BYTES - (size_t)off;

    _IO_SET_READ_NBYTES(ctp, n);
    if (n == 0) return _RESMGR_NPARTS(0);

    SETIOV(ctp->iov, flash + off, n);
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
    if (off >= (off_t)FLASH_BYTES) return ENOSPC;
    if ((size_t)off + n > FLASH_BYTES) n = FLASH_BYTES - (size_t)off;

    uint8_t buf[512];
    size_t done = 0;
    while (done < n) {
        size_t c = n - done;
        if (c > sizeof(buf)) c = sizeof(buf);
        ssize_t got = resmgr_msgread(ctp, buf, c, sizeof(msg->i) + done);
        if (got != (ssize_t)c) return EIO;
        for (size_t i = 0; i < c; i++) {
            flash[(size_t)off + done + i] &= buf[i]; /* NOR: program only clears bits */
        }
        done += c;
    }

    _IO_SET_WRITE_NBYTES(ctp, n);
    ocb->offset += n;
    if (n) ocb->attr->flags |= IOFUNC_ATTR_MTIME | IOFUNC_ATTR_CTIME;
    return _RESMGR_NPARTS(0);
}

static int io_devctl(resmgr_context_t *ctp, io_devctl_t *msg, iofunc_ocb_t *ocb)
{
    int status = iofunc_devctl_default(ctp, msg, ocb);
    if (status != _RESMGR_DEFAULT) return status;

    void *data = _DEVCTL_DATA(msg->i);

    switch (msg->i.dcmd) {
    case FF_DCMD_GEOMETRY: {
        ff_geometry_t g;
        g.total_bytes  = FLASH_BYTES;
        g.sector_bytes = SECTOR_BYTES;
        g.page_bytes   = PAGE_BYTES;
        g.sector_count = FLASH_BYTES / SECTOR_BYTES;
        memcpy(data, &g, sizeof(g));
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = 0;
        msg->o.nbytes  = sizeof(g);
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o) + sizeof(g));
    }
    case FF_DCMD_ERASE: {
        ff_erase_t er;
        memcpy(&er, data, sizeof(er));
        if (er.sector >= FLASH_BYTES / SECTOR_BYTES) return EINVAL;
        memset(flash + (size_t)er.sector * SECTOR_BYTES, 0xFF, SECTOR_BYTES);
        memset(&msg->o, 0, sizeof(msg->o));
        msg->o.ret_val = 0;
        return _RESMGR_PTR(ctp, &msg->o, sizeof(msg->o));
    }
    default:
        return ENOSYS;
    }
}

int main(void)
{
    memset(flash, 0xFF, sizeof(flash)); /* erased state */

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
    attr.nbytes = FLASH_BYTES;

    if (resmgr_attach(dpp, &rattr, FF_DEV_PATH, _FTYPE_ANY, 0,
                      &connect_funcs, &io_funcs, &attr) == -1) {
        perror("resmgr_attach");
        return 1;
    }

    printf("flashforged: %s ready (%u KiB, %u-byte sectors)\n",
           FF_DEV_PATH, FLASH_BYTES / 1024u, SECTOR_BYTES);

    dispatch_context_t *ctx = dispatch_context_alloc(dpp);
    for (;;) {
        ctx = dispatch_block(ctx);
        if (ctx == NULL) { perror("dispatch_block"); return 1; }
        dispatch_handler(ctx);
    }
}
