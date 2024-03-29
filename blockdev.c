/*
 * QEMU host block devices
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * later.  See the COPYING file in the top-level directory.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "sysemu/blockdev.h"
#include "hw/block/block.h"
#include "block/blockjob.h"
#include "monitor/monitor.h"
#include "qapi/qmp/qerror.h"
#include "qemu/option.h"
#include "qemu/config-file.h"
#include "qapi/qmp/types.h"
#include "qapi-visit.h"
#include "qapi/qmp-output-visitor.h"
#include "sysemu/sysemu.h"
#include "block/block_int.h"
#include "qmp-commands.h"
#include "trace.h"
#include "sysemu/arch_init.h"

static QTAILQ_HEAD(drivelist, DriveInfo) drives = QTAILQ_HEAD_INITIALIZER(drives);

static const char *const if_name[IF_COUNT] = {
    [IF_NONE] = "none",
    [IF_IDE] = "ide",
    [IF_SCSI] = "scsi",
    [IF_FLOPPY] = "floppy",
    [IF_PFLASH] = "pflash",
    [IF_MTD] = "mtd",
    [IF_SD] = "sd",
    [IF_VIRTIO] = "virtio",
    [IF_XEN] = "xen",
};

static const int if_max_devs[IF_COUNT] = {
    /*
     * Do not change these numbers!  They govern how drive option
     * index maps to unit and bus.  That mapping is ABI.
     *
     * All controllers used to imlement if=T drives need to support
     * if_max_devs[T] units, for any T with if_max_devs[T] != 0.
     * Otherwise, some index values map to "impossible" bus, unit
     * values.
     *
     * For instance, if you change [IF_SCSI] to 255, -drive
     * if=scsi,index=12 no longer means bus=1,unit=5, but
     * bus=0,unit=12.  With an lsi53c895a controller (7 units max),
     * the drive can't be set up.  Regression.
     */
    [IF_IDE] = 2,
    [IF_SCSI] = 7,
};

/*
 * We automatically delete the drive when a device using it gets
 * unplugged.  Questionable feature, but we can't just drop it.
 * Device models call blockdev_mark_auto_del() to schedule the
 * automatic deletion, and generic qdev code calls blockdev_auto_del()
 * when deletion is actually safe.
 */
void blockdev_mark_auto_del(BlockDriverState *bs)
{
    DriveInfo *dinfo = drive_get_by_blockdev(bs);

    if (dinfo && !dinfo->enable_auto_del) {
        return;
    }

    if (bs->job) {
        block_job_cancel(bs->job);
    }
    if (dinfo) {
        dinfo->auto_del = 1;
    }
}

void blockdev_auto_del(BlockDriverState *bs)
{
    DriveInfo *dinfo = drive_get_by_blockdev(bs);

    if (dinfo && dinfo->auto_del) {
        drive_put_ref(dinfo);
    }
}

static int drive_index_to_bus_id(BlockInterfaceType type, int index)
{
    int max_devs = if_max_devs[type];
    return max_devs ? index / max_devs : 0;
}

static int drive_index_to_unit_id(BlockInterfaceType type, int index)
{
    int max_devs = if_max_devs[type];
    return max_devs ? index % max_devs : index;
}

QemuOpts *drive_def(const char *optstr)
{
    return qemu_opts_parse(qemu_find_opts("drive"), optstr, 0);
}

QemuOpts *drive_add(BlockInterfaceType type, int index, const char *file,
                    const char *optstr)
{
    QemuOpts *opts;
    char buf[32];

    opts = drive_def(optstr);
    if (!opts) {
        return NULL;
    }
    if (type != IF_DEFAULT) {
        qemu_opt_set(opts, "if", if_name[type]);
    }
    if (index >= 0) {
        snprintf(buf, sizeof(buf), "%d", index);
        qemu_opt_set(opts, "index", buf);
    }
    if (file)
        qemu_opt_set(opts, "file", file);
    return opts;
}

DriveInfo *drive_get(BlockInterfaceType type, int bus, int unit)
{
    DriveInfo *dinfo;

    /* seek interface, bus and unit */

    QTAILQ_FOREACH(dinfo, &drives, next) {
        if (dinfo->type == type &&
	    dinfo->bus == bus &&
	    dinfo->unit == unit)
            return dinfo;
    }

    return NULL;
}

DriveInfo *drive_get_by_index(BlockInterfaceType type, int index)
{
    return drive_get(type,
                     drive_index_to_bus_id(type, index),
                     drive_index_to_unit_id(type, index));
}

int drive_get_max_bus(BlockInterfaceType type)
{
    int max_bus;
    DriveInfo *dinfo;

    max_bus = -1;
    QTAILQ_FOREACH(dinfo, &drives, next) {
        if(dinfo->type == type &&
           dinfo->bus > max_bus)
            max_bus = dinfo->bus;
    }
    return max_bus;
}

/* Get a block device.  This should only be used for single-drive devices
   (e.g. SD/Floppy/MTD).  Multi-disk devices (scsi/ide) should use the
   appropriate bus.  */
DriveInfo *drive_get_next(BlockInterfaceType type)
{
    static int next_block_unit[IF_COUNT];

    return drive_get(type, 0, next_block_unit[type]++);
}

DriveInfo *drive_get_by_blockdev(BlockDriverState *bs)
{
    DriveInfo *dinfo;

    QTAILQ_FOREACH(dinfo, &drives, next) {
        if (dinfo->bdrv == bs) {
            return dinfo;
        }
    }
    return NULL;
}

static void bdrv_format_print(void *opaque, const char *name)
{
    error_printf(" %s", name);
}

static void drive_uninit(DriveInfo *dinfo)
{
    if (dinfo->opts) {
        qemu_opts_del(dinfo->opts);
    }

    bdrv_unref(dinfo->bdrv);
    g_free(dinfo->id);
    QTAILQ_REMOVE(&drives, dinfo, next);
    g_free(dinfo->serial);
    g_free(dinfo);
}

void drive_put_ref(DriveInfo *dinfo)
{
    assert(dinfo->refcount);
    if (--dinfo->refcount == 0) {
        drive_uninit(dinfo);
    }
}

void drive_get_ref(DriveInfo *dinfo)
{
    dinfo->refcount++;
}

typedef struct {
    QEMUBH *bh;
    BlockDriverState *bs;
} BDRVPutRefBH;

/* right now, this is only used from block_job_cb() */
#ifdef CONFIG_LIVE_BLOCK_OPS
static void bdrv_put_ref_bh(void *opaque)
{
    BDRVPutRefBH *s = opaque;

    bdrv_unref(s->bs);
    qemu_bh_delete(s->bh);
    g_free(s);
}

/*
 * Release a BDS reference in a BH
 *
 * It is not safe to use bdrv_unref() from a callback function when the callers
 * still need the BlockDriverState.  In such cases we schedule a BH to release
 * the reference.
 */
static void bdrv_put_ref_bh_schedule(BlockDriverState *bs)
{
    BDRVPutRefBH *s;

    s = g_new(BDRVPutRefBH, 1);
    s->bh = qemu_bh_new(bdrv_put_ref_bh, s);
    s->bs = bs;
    qemu_bh_schedule(s->bh);
}
#endif

static int parse_block_error_action(const char *buf, bool is_read, Error **errp)
{
    if (!strcmp(buf, "ignore")) {
        return BLOCKDEV_ON_ERROR_IGNORE;
    } else if (!is_read && !strcmp(buf, "enospc")) {
        return BLOCKDEV_ON_ERROR_ENOSPC;
    } else if (!strcmp(buf, "stop")) {
        return BLOCKDEV_ON_ERROR_STOP;
    } else if (!strcmp(buf, "report")) {
        return BLOCKDEV_ON_ERROR_REPORT;
    } else {
        error_setg(errp, "'%s' invalid %s error action",
                   buf, is_read ? "read" : "write");
        return -1;
    }
}

static bool do_check_io_limits(BlockIOLimit *io_limits, Error **errp)
{
    bool bps_flag;
    bool iops_flag;

    assert(io_limits);

    bps_flag  = (io_limits->bps[BLOCK_IO_LIMIT_TOTAL] != 0)
                 && ((io_limits->bps[BLOCK_IO_LIMIT_READ] != 0)
                 || (io_limits->bps[BLOCK_IO_LIMIT_WRITE] != 0));
    iops_flag = (io_limits->iops[BLOCK_IO_LIMIT_TOTAL] != 0)
                 && ((io_limits->iops[BLOCK_IO_LIMIT_READ] != 0)
                 || (io_limits->iops[BLOCK_IO_LIMIT_WRITE] != 0));
    if (bps_flag || iops_flag) {
        error_setg(errp, "bps(iops) and bps_rd/bps_wr(iops_rd/iops_wr) "
                         "cannot be used at the same time");
        return false;
    }

    if (io_limits->bps[BLOCK_IO_LIMIT_TOTAL] < 0 ||
        io_limits->bps[BLOCK_IO_LIMIT_WRITE] < 0 ||
        io_limits->bps[BLOCK_IO_LIMIT_READ] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_TOTAL] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_WRITE] < 0 ||
        io_limits->iops[BLOCK_IO_LIMIT_READ] < 0) {
        error_setg(errp, "bps and iops values must be 0 or greater");
        return false;
    }

    return true;
}

typedef enum { MEDIA_DISK, MEDIA_CDROM } DriveMediaType;

/* Takes the ownership of bs_opts */
static DriveInfo *blockdev_init(QDict *bs_opts,
                                BlockInterfaceType type,
                                Error **errp)
{
    const char *buf;
    const char *file = NULL;
    const char *serial;
    int ro = 0;
    int bdrv_flags = 0;
    int on_read_error, on_write_error;
    DriveInfo *dinfo;
    BlockIOLimit io_limits;
    int snapshot = 0;
    bool copy_on_read;
    int ret;
    Error *error = NULL;
    QemuOpts *opts;
    const char *id;
    bool has_driver_specific_opts;
    BlockDriver *drv = NULL;

    /* Check common options by copying from bs_opts to opts, all other options
     * stay in bs_opts for processing by bdrv_open(). */
    id = qdict_get_try_str(bs_opts, "id");
    opts = qemu_opts_create(&qemu_common_drive_opts, id, 1, &error);
    if (error_is_set(&error)) {
        error_propagate(errp, error);
        return NULL;
    }

    qemu_opts_absorb_qdict(opts, bs_opts, &error);
    if (error_is_set(&error)) {
        error_propagate(errp, error);
        goto early_err;
    }

    if (id) {
        qdict_del(bs_opts, "id");
    }

    has_driver_specific_opts = !!qdict_size(bs_opts);

    /* extract parameters */
    snapshot = qemu_opt_get_bool(opts, "snapshot", 0);
    ro = qemu_opt_get_bool(opts, "read-only", 0);
    copy_on_read = qemu_opt_get_bool(opts, "copy-on-read", false);

    file = qemu_opt_get(opts, "file");
    serial = qemu_opt_get(opts, "serial");

    if ((buf = qemu_opt_get(opts, "discard")) != NULL) {
        if (bdrv_parse_discard_flags(buf, &bdrv_flags) != 0) {
            error_setg(errp, "invalid discard option");
            goto early_err;
        }
    }

    if (qemu_opt_get_bool(opts, "cache.writeback", true)) {
        bdrv_flags |= BDRV_O_CACHE_WB;
    }
    if (qemu_opt_get_bool(opts, "cache.direct", false)) {
        bdrv_flags |= BDRV_O_NOCACHE;
    }
    if (qemu_opt_get_bool(opts, "cache.no-flush", false)) {
        bdrv_flags |= BDRV_O_NO_FLUSH;
    }

#ifdef CONFIG_LINUX_AIO
    if ((buf = qemu_opt_get(opts, "aio")) != NULL) {
        if (!strcmp(buf, "native")) {
            bdrv_flags |= BDRV_O_NATIVE_AIO;
        } else if (!strcmp(buf, "threads")) {
            /* this is the default */
        } else {
           error_setg(errp, "invalid aio option");
           goto early_err;
        }
    }
#endif

    if ((buf = qemu_opt_get(opts, "format")) != NULL) {
        if (is_help_option(buf)) {
            error_printf("Supported formats:");
            bdrv_iterate_format(bdrv_format_print, NULL);
            error_printf("\n");
            goto early_err;
        }

        drv = bdrv_find_format(buf);
        if (!drv) {
            error_setg(errp, "'%s' invalid format", buf);
            goto early_err;
        }
    }

    /* disk I/O throttling */
    io_limits.bps[BLOCK_IO_LIMIT_TOTAL]  =
        qemu_opt_get_number(opts, "throttling.bps-total", 0);
    io_limits.bps[BLOCK_IO_LIMIT_READ]   =
        qemu_opt_get_number(opts, "throttling.bps-read", 0);
    io_limits.bps[BLOCK_IO_LIMIT_WRITE]  =
        qemu_opt_get_number(opts, "throttling.bps-write", 0);
    io_limits.iops[BLOCK_IO_LIMIT_TOTAL] =
        qemu_opt_get_number(opts, "throttling.iops-total", 0);
    io_limits.iops[BLOCK_IO_LIMIT_READ]  =
        qemu_opt_get_number(opts, "throttling.iops-read", 0);
    io_limits.iops[BLOCK_IO_LIMIT_WRITE] =
        qemu_opt_get_number(opts, "throttling.iops-write", 0);

    if (!do_check_io_limits(&io_limits, &error)) {
        error_propagate(errp, error);
        goto early_err;
    }

    on_write_error = BLOCKDEV_ON_ERROR_ENOSPC;
    if ((buf = qemu_opt_get(opts, "werror")) != NULL) {
        if (type != IF_IDE && type != IF_SCSI && type != IF_VIRTIO && type != IF_NONE) {
            error_setg(errp, "werror is not supported by this bus type");
            goto early_err;
        }

        on_write_error = parse_block_error_action(buf, 0, &error);
        if (error_is_set(&error)) {
            error_propagate(errp, error);
            goto early_err;
        }
    }

    on_read_error = BLOCKDEV_ON_ERROR_REPORT;
    if ((buf = qemu_opt_get(opts, "rerror")) != NULL) {
        if (type != IF_IDE && type != IF_VIRTIO && type != IF_SCSI && type != IF_NONE) {
            error_report("rerror is not supported by this bus type");
            goto early_err;
        }

        on_read_error = parse_block_error_action(buf, 1, &error);
        if (error_is_set(&error)) {
            error_propagate(errp, error);
            goto early_err;
        }
    }

    /* init */
    dinfo = g_malloc0(sizeof(*dinfo));
    dinfo->id = g_strdup(qemu_opts_id(opts));
    dinfo->bdrv = bdrv_new(dinfo->id, &error);
    if (error) {
        error_propagate(errp, error);
        goto bdrv_new_err;
    }
    dinfo->bdrv->open_flags = snapshot ? BDRV_O_SNAPSHOT : 0;
    dinfo->bdrv->read_only = ro;
    dinfo->type = type;
    dinfo->refcount = 1;
    if (serial != NULL) {
        dinfo->serial = g_strdup(serial);
    }
    QTAILQ_INSERT_TAIL(&drives, dinfo, next);

    bdrv_set_on_error(dinfo->bdrv, on_read_error, on_write_error);

    /* disk I/O throttling */
    bdrv_set_io_limits(dinfo->bdrv, &io_limits);

    if (!file || !*file) {
        if (has_driver_specific_opts) {
            file = NULL;
        } else {
            QDECREF(bs_opts);
            qemu_opts_del(opts);
            return dinfo;
        }
    }
    if (snapshot) {
        /* always use cache=unsafe with snapshot */
        bdrv_flags &= ~BDRV_O_CACHE_MASK;
        bdrv_flags |= (BDRV_O_SNAPSHOT|BDRV_O_CACHE_WB|BDRV_O_NO_FLUSH);
    }

    if (copy_on_read) {
        bdrv_flags |= BDRV_O_COPY_ON_READ;
    }

    if (runstate_check(RUN_STATE_INMIGRATE)) {
        bdrv_flags |= BDRV_O_INCOMING;
    }

    bdrv_flags |= ro ? 0 : BDRV_O_RDWR;

    QINCREF(bs_opts);
    ret = bdrv_open(dinfo->bdrv, file, bs_opts, bdrv_flags, drv, &error);

    if (ret < 0) {
        error_setg(errp, "could not open disk image %s: %s",
                   file ?: dinfo->id, error_get_pretty(error));
        error_free(error);
        goto err;
    }

    if (bdrv_key_required(dinfo->bdrv))
        autostart = 0;

    QDECREF(bs_opts);
    qemu_opts_del(opts);

    return dinfo;

err:
    bdrv_unref(dinfo->bdrv);
    QTAILQ_REMOVE(&drives, dinfo, next);
bdrv_new_err:
    g_free(dinfo->id);
    g_free(dinfo);
early_err:
    QDECREF(bs_opts);
    qemu_opts_del(opts);
    return NULL;
}

static void qemu_opt_rename(QemuOpts *opts, const char *from, const char *to)
{
    const char *value;

    value = qemu_opt_get(opts, from);
    if (value) {
        qemu_opt_set(opts, to, value);
        qemu_opt_unset(opts, from);
    }
}

QemuOptsList qemu_legacy_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_legacy_drive_opts.head),
    .desc = {
        {
            .name = "bus",
            .type = QEMU_OPT_NUMBER,
            .help = "bus number",
        },{
            .name = "unit",
            .type = QEMU_OPT_NUMBER,
            .help = "unit number (i.e. lun for scsi)",
        },{
            .name = "index",
            .type = QEMU_OPT_NUMBER,
            .help = "index number",
        },{
            .name = "media",
            .type = QEMU_OPT_STRING,
            .help = "media type (disk, cdrom)",
        },{
            .name = "if",
            .type = QEMU_OPT_STRING,
            .help = "interface (ide, scsi, sd, mtd, floppy, pflash, virtio)",
        },{
            .name = "cyls",
            .type = QEMU_OPT_NUMBER,
            .help = "number of cylinders (ide disk geometry)",
        },{
            .name = "heads",
            .type = QEMU_OPT_NUMBER,
            .help = "number of heads (ide disk geometry)",
        },{
            .name = "secs",
            .type = QEMU_OPT_NUMBER,
            .help = "number of sectors (ide disk geometry)",
        },{
            .name = "trans",
            .type = QEMU_OPT_STRING,
            .help = "chs translation (auto, lba, none)",
        },{
            .name = "boot",
            .type = QEMU_OPT_BOOL,
            .help = "(deprecated, ignored)",
        },{
            .name = "addr",
            .type = QEMU_OPT_STRING,
            .help = "pci address (virtio only)",
        },

        /* Options that are passed on, but have special semantics with -drive */
        {
            .name = "read-only",
            .type = QEMU_OPT_BOOL,
            .help = "open drive file as read-only",
        },{
            .name = "copy-on-read",
            .type = QEMU_OPT_BOOL,
            .help = "copy read data from backing file into image file",
        },

        { /* end of list */ }
    },
};

DriveInfo *drive_init(QemuOpts *all_opts, BlockInterfaceType block_default_type)
{
    const char *value;
    DriveInfo *dinfo = NULL;
    QDict *bs_opts;
    QemuOpts *legacy_opts;
    DriveMediaType media = MEDIA_DISK;
    BlockInterfaceType type;
    int cyls, heads, secs, translation;
    int max_devs, bus_id, unit_id, index;
    const char *devaddr;
    bool read_only = false;
    bool copy_on_read;
    Error *local_err = NULL;

    /* Change legacy command line options into QMP ones */
    qemu_opt_rename(all_opts, "iops", "throttling.iops-total");
    qemu_opt_rename(all_opts, "iops_rd", "throttling.iops-read");
    qemu_opt_rename(all_opts, "iops_wr", "throttling.iops-write");

    qemu_opt_rename(all_opts, "bps", "throttling.bps-total");
    qemu_opt_rename(all_opts, "bps_rd", "throttling.bps-read");
    qemu_opt_rename(all_opts, "bps_wr", "throttling.bps-write");

    qemu_opt_rename(all_opts, "readonly", "read-only");

    value = qemu_opt_get(all_opts, "cache");
    if (value) {
        int flags = 0;

        if (bdrv_parse_cache_flags(value, &flags) != 0) {
            error_report("invalid cache option");
            return NULL;
        }

        /* Specific options take precedence */
        if (!qemu_opt_get(all_opts, "cache.writeback")) {
            qemu_opt_set_bool(all_opts, "cache.writeback",
                              !!(flags & BDRV_O_CACHE_WB));
        }
        if (!qemu_opt_get(all_opts, "cache.direct")) {
            qemu_opt_set_bool(all_opts, "cache.direct",
                              !!(flags & BDRV_O_NOCACHE));
        }
        if (!qemu_opt_get(all_opts, "cache.no-flush")) {
            qemu_opt_set_bool(all_opts, "cache.no-flush",
                              !!(flags & BDRV_O_NO_FLUSH));
        }
        qemu_opt_unset(all_opts, "cache");
    }

    /* Get a QDict for processing the options */
    bs_opts = qdict_new();
    qemu_opts_to_qdict(all_opts, bs_opts);

    legacy_opts = qemu_opts_create_nofail(&qemu_legacy_drive_opts);
    qemu_opts_absorb_qdict(legacy_opts, bs_opts, &local_err);
    if (error_is_set(&local_err)) {
        qerror_report_err(local_err);
        error_free(local_err);
        goto fail;
    }

    /* Deprecated option boot=[on|off] */
    if (qemu_opt_get(legacy_opts, "boot") != NULL) {
        fprintf(stderr, "qemu-kvm: boot=on|off is deprecated and will be "
                "ignored. Future versions will reject this parameter. Please "
                "update your scripts.\n");
    }

    /* Media type */
    value = qemu_opt_get(legacy_opts, "media");
    if (value) {
        if (!strcmp(value, "disk")) {
            media = MEDIA_DISK;
        } else if (!strcmp(value, "cdrom")) {
            media = MEDIA_CDROM;
            read_only = true;
        } else {
            error_report("'%s' invalid media", value);
            goto fail;
        }
    }

    /* copy-on-read is disabled with a warning for read-only devices */
    read_only |= qemu_opt_get_bool(legacy_opts, "read-only", false);
    copy_on_read = qemu_opt_get_bool(legacy_opts, "copy-on-read", false);

    if (read_only && copy_on_read) {
        error_report("warning: disabling copy-on-read on read-only drive");
        copy_on_read = false;
    }

    qdict_put(bs_opts, "read-only",
              qstring_from_str(read_only ? "on" : "off"));
    qdict_put(bs_opts, "copy-on-read",
              qstring_from_str(copy_on_read ? "on" :"off"));

    /* Controller type */
    value = qemu_opt_get(legacy_opts, "if");
    if (value) {
        for (type = 0;
             type < IF_COUNT && strcmp(value, if_name[type]);
             type++) {
        }
        if (type == IF_COUNT) {
            error_report("unsupported bus type '%s'", value);
            goto fail;
        }
    } else {
        type = block_default_type;
    }

    /* Geometry */
    cyls  = qemu_opt_get_number(legacy_opts, "cyls", 0);
    heads = qemu_opt_get_number(legacy_opts, "heads", 0);
    secs  = qemu_opt_get_number(legacy_opts, "secs", 0);

    if (cyls || heads || secs) {
        if (cyls < 1) {
            error_report("invalid physical cyls number");
            goto fail;
        }
        if (heads < 1) {
            error_report("invalid physical heads number");
            goto fail;
        }
        if (secs < 1) {
            error_report("invalid physical secs number");
            goto fail;
        }
    }

    translation = BIOS_ATA_TRANSLATION_AUTO;
    value = qemu_opt_get(legacy_opts, "trans");
    if (value != NULL) {
        if (!cyls) {
            error_report("'%s' trans must be used with cyls, heads and secs",
                         value);
            goto fail;
        }
        if (!strcmp(value, "none")) {
            translation = BIOS_ATA_TRANSLATION_NONE;
        } else if (!strcmp(value, "lba")) {
            translation = BIOS_ATA_TRANSLATION_LBA;
        } else if (!strcmp(value, "auto")) {
            translation = BIOS_ATA_TRANSLATION_AUTO;
        } else {
            error_report("'%s' invalid translation type", value);
            goto fail;
        }
    }

    if (media == MEDIA_CDROM) {
        if (cyls || secs || heads) {
            error_report("CHS can't be set with media=cdrom");
            goto fail;
        }
    }

    /* Device address specified by bus/unit or index.
     * If none was specified, try to find the first free one. */
    bus_id  = qemu_opt_get_number(legacy_opts, "bus", 0);
    unit_id = qemu_opt_get_number(legacy_opts, "unit", -1);
    index   = qemu_opt_get_number(legacy_opts, "index", -1);

    max_devs = if_max_devs[type];

    if (index != -1) {
        if (bus_id != 0 || unit_id != -1) {
            error_report("index cannot be used with bus and unit");
            goto fail;
        }
        bus_id = drive_index_to_bus_id(type, index);
        unit_id = drive_index_to_unit_id(type, index);
    }

    if (unit_id == -1) {
       unit_id = 0;
       while (drive_get(type, bus_id, unit_id) != NULL) {
           unit_id++;
           if (max_devs && unit_id >= max_devs) {
               unit_id -= max_devs;
               bus_id++;
           }
       }
    }

    if (max_devs && unit_id >= max_devs) {
        error_report("unit %d too big (max is %d)", unit_id, max_devs - 1);
        goto fail;
    }

    if (drive_get(type, bus_id, unit_id) != NULL) {
        error_report("drive with bus=%d, unit=%d (index=%d) exists",
                     bus_id, unit_id, index);
        goto fail;
    }

    /* no id supplied -> create one */
    if (qemu_opts_id(all_opts) == NULL) {
        char *new_id;
        const char *mediastr = "";
        if (type == IF_IDE || type == IF_SCSI) {
            mediastr = (media == MEDIA_CDROM) ? "-cd" : "-hd";
        }
        if (max_devs) {
            new_id = g_strdup_printf("%s%i%s%i", if_name[type], bus_id,
                                     mediastr, unit_id);
        } else {
            new_id = g_strdup_printf("%s%s%i", if_name[type],
                                     mediastr, unit_id);
        }
        qdict_put(bs_opts, "id", qstring_from_str(new_id));
        g_free(new_id);
    }

    /* Add virtio block device */
    devaddr = qemu_opt_get(legacy_opts, "addr");
    if (devaddr && type != IF_VIRTIO) {
        error_report("addr is not supported by this bus type");
        goto fail;
    }

    if (type == IF_VIRTIO) {
        QemuOpts *devopts;
        devopts = qemu_opts_create_nofail(qemu_find_opts("device"));
        if (arch_type == QEMU_ARCH_S390X) {
            qemu_opt_set(devopts, "driver", "virtio-blk-s390");
        } else {
            qemu_opt_set(devopts, "driver", "virtio-blk-pci");
        }
        qemu_opt_set(devopts, "drive", qdict_get_str(bs_opts, "id"));
        if (devaddr) {
            qemu_opt_set(devopts, "addr", devaddr);
        }
    }

    /* Actual block device init: Functionality shared with blockdev-add */
    dinfo = blockdev_init(bs_opts, type, &local_err);
    if (dinfo == NULL) {
        if (error_is_set(&local_err)) {
            qerror_report_err(local_err);
            error_free(local_err);
        }
        goto fail;
    } else {
        assert(!error_is_set(&local_err));
    }

    /* Set legacy DriveInfo fields */
    dinfo->enable_auto_del = true;
    dinfo->opts = all_opts;

    dinfo->cyls = cyls;
    dinfo->heads = heads;
    dinfo->secs = secs;
    dinfo->trans = translation;

    dinfo->bus = bus_id;
    dinfo->unit = unit_id;
    dinfo->devaddr = devaddr;

    switch(type) {
    case IF_IDE:
    case IF_SCSI:
    case IF_XEN:
    case IF_NONE:
        dinfo->media_cd = media == MEDIA_CDROM;
        break;
    default:
        break;
    }

fail:
    qemu_opts_del(legacy_opts);
    return dinfo;
}

void do_commit(Monitor *mon, const QDict *qdict)
{
    const char *device = qdict_get_str(qdict, "device");
    BlockDriverState *bs;
    int ret;

    if (!strcmp(device, "all")) {
        ret = bdrv_commit_all();
    } else {
        bs = bdrv_find(device);
        if (!bs) {
            monitor_printf(mon, "Device '%s' not found\n", device);
            return;
        }
        ret = bdrv_commit(bs);
    }
    if (ret < 0) {
        monitor_printf(mon, "'commit' error for '%s': %s\n", device,
                       strerror(-ret));
    }
}

#ifdef CONFIG_LIVE_BLOCK_OPS
static void blockdev_do_action(int kind, void *data, Error **errp)
{
    BlockdevAction action;
    BlockdevActionList list;

    action.kind = kind;
    action.data = data;
    list.value = &action;
    list.next = NULL;
    qmp_transaction(&list, errp);
}

void qmp_blockdev_snapshot_sync(const char *device, const char *snapshot_file,
                                bool has_format, const char *format,
                                bool has_mode, enum NewImageMode mode,
                                Error **errp)
{
    BlockdevSnapshot snapshot = {
        .device = (char *) device,
        .snapshot_file = (char *) snapshot_file,
        .has_format = has_format,
        .format = (char *) format,
        .has_mode = has_mode,
        .mode = mode,
    };
    blockdev_do_action(BLOCKDEV_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC, &snapshot,
                       errp);
}


/* New and old BlockDriverState structs for group snapshots */

typedef struct BlkTransactionStates BlkTransactionStates;

/* Only prepare() may fail. In a single transaction, only one of commit() or
   abort() will be called, clean() will always be called if it present. */
typedef struct BdrvActionOps {
    /* Size of state struct, in bytes. */
    size_t instance_size;
    /* Prepare the work, must NOT be NULL. */
    void (*prepare)(BlkTransactionStates *common, Error **errp);
    /* Commit the changes, must NOT be NULL. */
    void (*commit)(BlkTransactionStates *common);
    /* Abort the changes on fail, can be NULL. */
    void (*abort)(BlkTransactionStates *common);
    /* Clean up resource in the end, can be NULL. */
    void (*clean)(BlkTransactionStates *common);
} BdrvActionOps;

/*
 * This structure must be arranged as first member in child type, assuming
 * that compiler will also arrange it to the same address with parent instance.
 * Later it will be used in free().
 */
struct BlkTransactionStates {
    BlockdevAction *action;
    const BdrvActionOps *ops;
    QSIMPLEQ_ENTRY(BlkTransactionStates) entry;
};

/* external snapshot private data */
typedef struct ExternalSnapshotStates {
    BlkTransactionStates common;
    BlockDriverState *old_bs;
    BlockDriverState *new_bs;
} ExternalSnapshotStates;

static void external_snapshot_prepare(BlkTransactionStates *common,
                                      Error **errp)
{
    BlockDriver *drv;
    int flags, ret;
    Error *local_err = NULL;
    const char *device;
    const char *new_image_file;
    const char *format = "qcow2";
    enum NewImageMode mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    ExternalSnapshotStates *states =
                             DO_UPCAST(ExternalSnapshotStates, common, common);
    BlockdevAction *action = common->action;

    /* get parameters */
    g_assert(action->kind == BLOCKDEV_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC);

    device = action->blockdev_snapshot_sync->device;
    new_image_file = action->blockdev_snapshot_sync->snapshot_file;
    if (action->blockdev_snapshot_sync->has_format) {
        format = action->blockdev_snapshot_sync->format;
    }
    if (action->blockdev_snapshot_sync->has_mode) {
        mode = action->blockdev_snapshot_sync->mode;
    }

    /* start processing */
    drv = bdrv_find_format(format);
    if (!drv) {
        error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
        return;
    }

    states->old_bs = bdrv_find(device);
    if (!states->old_bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(states->old_bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (bdrv_in_use(states->old_bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, device);
        return;
    }

    if (!bdrv_is_read_only(states->old_bs)) {
        if (bdrv_flush(states->old_bs)) {
            error_set(errp, QERR_IO_ERROR);
            return;
        }
    }

    flags = states->old_bs->open_flags;

    /* create new image w/backing file */
    if (mode != NEW_IMAGE_MODE_EXISTING) {
        bdrv_img_create(new_image_file, format,
                        states->old_bs->filename,
                        states->old_bs->drv->format_name,
                        NULL, -1, flags, &local_err, false);
        if (error_is_set(&local_err)) {
            error_propagate(errp, local_err);
            return;
        }
    }

    /* We will manually add the backing_hd field to the bs later */
    states->new_bs = bdrv_new("", &error_abort);
    /* TODO Inherit bs->options or only take explicit options with an
     * extended QMP command? */
    ret = bdrv_open(states->new_bs, new_image_file, NULL,
                    flags | BDRV_O_NO_BACKING, drv, &local_err);
    if (ret != 0) {
        error_propagate(errp, local_err);
    }
}

static void external_snapshot_commit(BlkTransactionStates *common)
{
    ExternalSnapshotStates *states =
                             DO_UPCAST(ExternalSnapshotStates, common, common);

    /* This removes our old bs from the bdrv_states, and adds the new bs */
    bdrv_append(states->new_bs, states->old_bs);
    /* We don't need (or want) to use the transactional
     * bdrv_reopen_multiple() across all the entries at once, because we
     * don't want to abort all of them if one of them fails the reopen */
    bdrv_reopen(states->new_bs, states->new_bs->open_flags & ~BDRV_O_RDWR,
                NULL);
}

static void external_snapshot_abort(BlkTransactionStates *common)
{
    ExternalSnapshotStates *states =
                             DO_UPCAST(ExternalSnapshotStates, common, common);
    if (states->new_bs) {
        bdrv_unref(states->new_bs);
    }
}

static const BdrvActionOps actions[] = {
    [BLOCKDEV_ACTION_KIND_BLOCKDEV_SNAPSHOT_SYNC] = {
        .instance_size = sizeof(ExternalSnapshotStates),
        .prepare  = external_snapshot_prepare,
        .commit   = external_snapshot_commit,
        .abort = external_snapshot_abort,
    },
};

/*
 * 'Atomic' group snapshots.  The snapshots are taken as a set, and if any fail
 *  then we do not pivot any of the devices in the group, and abandon the
 *  snapshots
 */
void qmp_transaction(BlockdevActionList *dev_list, Error **errp)
{
    BlockdevActionList *dev_entry = dev_list;
    BlkTransactionStates *states, *next;
    Error *local_err = NULL;

    QSIMPLEQ_HEAD(snap_bdrv_states, BlkTransactionStates) snap_bdrv_states;
    QSIMPLEQ_INIT(&snap_bdrv_states);

    /* drain all i/o before any snapshots */
    bdrv_drain_all();

    /* We don't do anything in this loop that commits us to the snapshot */
    while (NULL != dev_entry) {
        BlockdevAction *dev_info = NULL;
        const BdrvActionOps *ops;

        dev_info = dev_entry->value;
        dev_entry = dev_entry->next;

        assert(dev_info->kind < ARRAY_SIZE(actions));

        ops = &actions[dev_info->kind];
        states = g_malloc0(ops->instance_size);
        states->ops = ops;
        states->action = dev_info;
        QSIMPLEQ_INSERT_TAIL(&snap_bdrv_states, states, entry);

        states->ops->prepare(states, &local_err);
        if (error_is_set(&local_err)) {
            error_propagate(errp, local_err);
            goto delete_and_fail;
        }
    }

    QSIMPLEQ_FOREACH(states, &snap_bdrv_states, entry) {
        states->ops->commit(states);
    }

    /* success */
    goto exit;

delete_and_fail:
    /*
    * failure, and it is all-or-none; abandon each new bs, and keep using
    * the original bs for all images
    */
    QSIMPLEQ_FOREACH(states, &snap_bdrv_states, entry) {
        if (states->ops->abort) {
            states->ops->abort(states);
        }
    }
exit:
    QSIMPLEQ_FOREACH_SAFE(states, &snap_bdrv_states, entry, next) {
        if (states->ops->clean) {
            states->ops->clean(states);
        }
        g_free(states);
    }
}
#endif


static void eject_device(BlockDriverState *bs, int force, Error **errp)
{
    if (bdrv_in_use(bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, bdrv_get_device_name(bs));
        return;
    }
    if (!bdrv_dev_has_removable_media(bs)) {
        error_set(errp, QERR_DEVICE_NOT_REMOVABLE, bdrv_get_device_name(bs));
        return;
    }

    if (bdrv_dev_is_medium_locked(bs) && !bdrv_dev_is_tray_open(bs)) {
        bdrv_dev_eject_request(bs, force);
        if (!force) {
            error_set(errp, QERR_DEVICE_LOCKED, bdrv_get_device_name(bs));
            return;
        }
    }

    bdrv_close(bs);
}

void qmp_eject(const char *device, bool has_force, bool force, Error **errp)
{
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    eject_device(bs, force, errp);
}

void qmp_block_passwd(const char *device, const char *password, Error **errp)
{
    BlockDriverState *bs;
    int err;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    err = bdrv_set_key(bs, password);
    if (err == -EINVAL) {
        error_set(errp, QERR_DEVICE_NOT_ENCRYPTED, bdrv_get_device_name(bs));
        return;
    } else if (err < 0) {
        error_set(errp, QERR_INVALID_PASSWORD);
        return;
    }
}

static void qmp_bdrv_open_encrypted(BlockDriverState *bs, const char *filename,
                                    int bdrv_flags, BlockDriver *drv,
                                    const char *password, Error **errp)
{
    Error *local_err = NULL;
    int ret;

    ret = bdrv_open(bs, filename, NULL, bdrv_flags, drv, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        return;
    }

    if (bdrv_key_required(bs)) {
        if (password) {
            if (bdrv_set_key(bs, password) < 0) {
                error_set(errp, QERR_INVALID_PASSWORD);
            }
        } else {
            error_set(errp, QERR_DEVICE_ENCRYPTED, bdrv_get_device_name(bs),
                      bdrv_get_encrypted_filename(bs));
        }
    } else if (password) {
        error_set(errp, QERR_DEVICE_NOT_ENCRYPTED, bdrv_get_device_name(bs));
    }
}

void qmp_change_blockdev(const char *device, const char *filename,
                         bool has_format, const char *format, Error **errp)
{
    BlockDriverState *bs;
    BlockDriver *drv = NULL;
    int bdrv_flags;
    Error *err = NULL;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (format) {
        drv = bdrv_find_whitelisted_format(format, bs->read_only);
        if (!drv) {
            error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
            return;
        }
    }

    eject_device(bs, 0, &err);
    if (error_is_set(&err)) {
        error_propagate(errp, err);
        return;
    }

    bdrv_flags = bdrv_is_read_only(bs) ? 0 : BDRV_O_RDWR;
    bdrv_flags |= bdrv_is_snapshot(bs) ? BDRV_O_SNAPSHOT : 0;

    qmp_bdrv_open_encrypted(bs, filename, bdrv_flags, drv, NULL, errp);
}

/* throttling disk I/O limits */
void qmp_block_set_io_throttle(const char *device, int64_t bps, int64_t bps_rd,
                               int64_t bps_wr, int64_t iops, int64_t iops_rd,
                               int64_t iops_wr, Error **errp)
{
    BlockIOLimit io_limits;
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    io_limits.bps[BLOCK_IO_LIMIT_TOTAL] = bps;
    io_limits.bps[BLOCK_IO_LIMIT_READ]  = bps_rd;
    io_limits.bps[BLOCK_IO_LIMIT_WRITE] = bps_wr;
    io_limits.iops[BLOCK_IO_LIMIT_TOTAL]= iops;
    io_limits.iops[BLOCK_IO_LIMIT_READ] = iops_rd;
    io_limits.iops[BLOCK_IO_LIMIT_WRITE]= iops_wr;

    if (!do_check_io_limits(&io_limits, errp)) {
        return;
    }

    bs->io_limits = io_limits;

    if (!bs->io_limits_enabled && bdrv_io_limits_enabled(bs)) {
        bdrv_io_limits_enable(bs);
    } else if (bs->io_limits_enabled && !bdrv_io_limits_enabled(bs)) {
        bdrv_io_limits_disable(bs);
    } else {
        if (bs->block_timer) {
            qemu_mod_timer(bs->block_timer, qemu_get_clock_ns(vm_clock));
        }
    }
}

int do_drive_del(Monitor *mon, const QDict *qdict, QObject **ret_data)
{
    const char *id = qdict_get_str(qdict, "id");
    BlockDriverState *bs;

    bs = bdrv_find(id);
    if (!bs) {
        qerror_report(QERR_DEVICE_NOT_FOUND, id);
        return -1;
    }
    if (bdrv_in_use(bs)) {
        qerror_report(QERR_DEVICE_IN_USE, id);
        return -1;
    }

    /* quiesce block driver; prevent further io */
    bdrv_drain_all();
    bdrv_flush(bs);
    bdrv_close(bs);

    /* if we have a device attached to this BlockDriverState
     * then we need to make the drive anonymous until the device
     * can be removed.  If this is a drive with no device backing
     * then we can just get rid of the block driver state right here.
     */
    if (bdrv_get_attached_dev(bs)) {
        bdrv_make_anon(bs);

        /* Further I/O must not pause the guest */
        bdrv_set_on_error(bs, BLOCKDEV_ON_ERROR_REPORT,
                          BLOCKDEV_ON_ERROR_REPORT);
    } else {
        drive_uninit(drive_get_by_blockdev(bs));
    }

    return 0;
}

void qmp_block_resize(const char *device, int64_t size, Error **errp)
{
    BlockDriverState *bs;
    int ret;

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (size < 0) {
        error_set(errp, QERR_INVALID_PARAMETER_VALUE, "size", "a >0 size");
        return;
    }

    /* complete all in-flight operations before resizing the device */
    bdrv_drain_all();

    ret = bdrv_truncate(bs, size);
    switch (ret) {
    case 0:
        break;
    case -ENOMEDIUM:
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        break;
    case -ENOTSUP:
        error_set(errp, QERR_UNSUPPORTED);
        break;
    case -EACCES:
        error_set(errp, QERR_DEVICE_IS_READ_ONLY, device);
        break;
    case -EBUSY:
        error_set(errp, QERR_DEVICE_IN_USE, device);
        break;
    default:
        error_setg_errno(errp, -ret, "Could not resize");
        break;
    }
}

#ifdef CONFIG_LIVE_BLOCK_OPS
static void block_job_cb(void *opaque, int ret)
{
    BlockDriverState *bs = opaque;
    QObject *obj;

    trace_block_job_cb(bs, bs->job, ret);

    assert(bs->job);
    obj = qobject_from_block_job(bs->job);
    if (ret < 0) {
        QDict *dict = qobject_to_qdict(obj);
        qdict_put(dict, "error", qstring_from_str(strerror(-ret)));
    }

    if (block_job_is_cancelled(bs->job)) {
        monitor_protocol_event(QEVENT_BLOCK_JOB_CANCELLED, obj);
    } else {
        monitor_protocol_event(QEVENT_BLOCK_JOB_COMPLETED, obj);
    }
    qobject_decref(obj);

    bdrv_put_ref_bh_schedule(bs);
}

void qmp_block_stream(const char *device,
                      bool has_base, const char *base,
                      bool has_backing_file, const char *backing_file,
                      bool has_speed, int64_t speed,
                      bool has_on_error, BlockdevOnError on_error,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *base_bs = NULL;
    Error *local_err = NULL;
    const char *base_name = NULL;

    if (!has_on_error) {
        on_error = BLOCKDEV_ON_ERROR_REPORT;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (has_base) {
        base_bs = bdrv_find_backing_image(bs, base);
        if (base_bs == NULL) {
            error_set(errp, QERR_BASE_NOT_FOUND, base);
            return;
        }
        base_name = base;
    }

    /* if we are streaming the entire chain, the result will have no backing
     * file, and specifying one is therefore an error */
    if (base_bs == NULL && has_backing_file) {
        error_setg(errp, "backing file specified, but streaming the "
                         "entire chain");
        return;
    }

    /* backing_file string overrides base bs filename */
    base_name = has_backing_file ? backing_file : base_name;

    stream_start(bs, base_bs, base_name, has_speed ? speed : 0,
                 on_error, block_job_cb, bs, &local_err);
    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    trace_qmp_block_stream(bs, bs->job);
}

void qmp_block_commit(const char *device,
                      bool has_base, const char *base,
                      bool has_top, const char *top,
                      bool has_backing_file, const char *backing_file,
                      bool has_speed, int64_t speed,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *base_bs, *top_bs;
    Error *local_err = NULL;
    /* This will be part of the QMP command, if/when the
     * BlockdevOnError change for blkmirror makes it in
     */
    BlockdevOnError on_error = BLOCKDEV_ON_ERROR_REPORT;

    /* drain all i/o before commits */
    bdrv_drain_all();

    /* Important Note:
     *  libvirt relies on the DeviceNotFound error class in order to probe for
     *  live commit feature versions; for this to work, we must make sure to
     *  perform the device lookup before any generic errors that may occur in a
     *  scenario in which all optional arguments are omitted. */
    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    /* default top_bs is the active layer */
    top_bs = bs;

    if (has_top && top) {
        if (strcmp(bs->filename, top) != 0) {
            top_bs = bdrv_find_backing_image(bs, top);
        }
    }

    if (top_bs == NULL) {
        error_setg(errp, "Top image file %s not found", top ? top : "NULL");
        return;
    }

    if (has_base && base) {
        base_bs = bdrv_find_backing_image(top_bs, base);
    } else {
        base_bs = bdrv_find_base(top_bs);
    }

    if (base_bs == NULL) {
        error_set(errp, QERR_BASE_NOT_FOUND, base ? base : "NULL");
        return;
    }

    /* Do not allow attempts to commit an image into itself */
    if (top_bs == base_bs) {
        error_setg(errp, "cannot commit an image into itself");
        return;
    }

    if (top_bs == bs) {
        if (has_backing_file) {
            error_setg(errp, "'backing-file' specified,"
                             " but 'top' is the active layer");
            return;
        }
        commit_active_start(bs, base_bs, speed, on_error, block_job_cb,
                            bs, &local_err);
    } else {
        commit_start(bs, base_bs, top_bs, speed, on_error, block_job_cb, bs,
                     has_backing_file ? backing_file : NULL, &local_err);
    }
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }
}

#define DEFAULT_MIRROR_BUF_SIZE   (10 << 20)

void qmp_drive_mirror(const char *device, const char *target,
                      bool has_format, const char *format,
                      enum MirrorSyncMode sync,
                      bool has_mode, enum NewImageMode mode,
                      bool has_speed, int64_t speed,
                      bool has_granularity, uint32_t granularity,
                      bool has_buf_size, int64_t buf_size,
                      bool has_on_source_error, BlockdevOnError on_source_error,
                      bool has_on_target_error, BlockdevOnError on_target_error,
                      Error **errp)
{
    BlockDriverState *bs;
    BlockDriverState *source, *target_bs;
    BlockDriver *drv = NULL;
    Error *local_err = NULL;
    int flags;
    int64_t size;
    int ret;

    if (!has_speed) {
        speed = 0;
    }
    if (!has_on_source_error) {
        on_source_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_on_target_error) {
        on_target_error = BLOCKDEV_ON_ERROR_REPORT;
    }
    if (!has_mode) {
        mode = NEW_IMAGE_MODE_ABSOLUTE_PATHS;
    }
    if (!has_granularity) {
        granularity = 0;
    }
    if (!has_buf_size) {
        buf_size = DEFAULT_MIRROR_BUF_SIZE;
    }

    if (granularity != 0 && (granularity < 512 || granularity > 1048576 * 64)) {
        error_set(errp, QERR_INVALID_PARAMETER, device);
        return;
    }
    if (granularity & (granularity - 1)) {
        error_set(errp, QERR_INVALID_PARAMETER, device);
        return;
    }

    bs = bdrv_find(device);
    if (!bs) {
        error_set(errp, QERR_DEVICE_NOT_FOUND, device);
        return;
    }

    if (!bdrv_is_inserted(bs)) {
        error_set(errp, QERR_DEVICE_HAS_NO_MEDIUM, device);
        return;
    }

    if (!has_format) {
        format = mode == NEW_IMAGE_MODE_EXISTING ? NULL : bs->drv->format_name;
    }
    if (format) {
        drv = bdrv_find_format(format);
        if (!drv) {
            error_set(errp, QERR_INVALID_BLOCK_FORMAT, format);
            return;
        }
    }

    if (bdrv_in_use(bs)) {
        error_set(errp, QERR_DEVICE_IN_USE, device);
        return;
    }

    flags = bs->open_flags | BDRV_O_RDWR;
    source = bs->backing_hd;
    if (!source && sync == MIRROR_SYNC_MODE_TOP) {
        sync = MIRROR_SYNC_MODE_FULL;
    }
    if (sync == MIRROR_SYNC_MODE_NONE) {
        source = bs;
    }

    size = bdrv_getlength(bs);
    if (size < 0) {
        error_setg_errno(errp, -size, "bdrv_getlength failed");
        return;
    }

    if ((sync == MIRROR_SYNC_MODE_FULL || !source)
        && mode != NEW_IMAGE_MODE_EXISTING)
    {
        /* create new image w/o backing file */
        assert(format && drv);
        bdrv_img_create(target, format,
                        NULL, NULL, NULL, size, flags, &local_err, false);
    } else {
        switch (mode) {
        case NEW_IMAGE_MODE_EXISTING:
            ret = 0;
            break;
        case NEW_IMAGE_MODE_ABSOLUTE_PATHS:
            /* create new image with backing file */
            bdrv_img_create(target, format,
                            source->filename,
                            source->drv->format_name,
                            NULL, size, flags, &local_err, false);
            break;
        default:
            abort();
        }
    }

    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        return;
    }

    /* Mirroring takes care of copy-on-write using the source's backing
     * file.
     */
    target_bs = bdrv_new("", &error_abort);
    ret = bdrv_open(target_bs, target, NULL, flags | BDRV_O_NO_BACKING, drv,
                    &local_err);
    if (ret < 0) {
        bdrv_unref(target_bs);
        error_propagate(errp, local_err);
        return;
    }

    mirror_start(bs, target_bs, speed, granularity, buf_size, sync,
                 on_source_error, on_target_error,
                 block_job_cb, bs, &local_err);
    if (local_err != NULL) {
        bdrv_unref(target_bs);
        error_propagate(errp, local_err);
        return;
    }
}

static BlockJob *find_block_job(const char *device)
{
    BlockDriverState *bs;

    bs = bdrv_find(device);
    if (!bs || !bs->job) {
        return NULL;
    }
    return bs->job;
}

void qmp_block_job_set_speed(const char *device, int64_t speed, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    block_job_set_speed(job, speed, errp);
}

void qmp_block_job_cancel(const char *device,
                          bool has_force, bool force, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!has_force) {
        force = false;
    }

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }
    if (job->paused && !force) {
        error_set(errp, QERR_BLOCK_JOB_PAUSED, device);
        return;
    }

    trace_qmp_block_job_cancel(job);
    block_job_cancel(job);
}

void qmp_block_job_pause(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_pause(job);
    block_job_pause(job);
}

void qmp_block_job_resume(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_resume(job);
    block_job_resume(job);
}

void qmp_block_job_complete(const char *device, Error **errp)
{
    BlockJob *job = find_block_job(device);

    if (!job) {
        error_set(errp, QERR_BLOCK_JOB_NOT_ACTIVE, device);
        return;
    }

    trace_qmp_block_job_complete(job);
    block_job_complete(job, errp);
}
#endif

void qmp___com_redhat_change_backing_file(const char *device,
                                          const char *image_node_name,
                                          const char *backing_file,
                                          Error **errp)
{
    error_set(errp, QERR_UNSUPPORTED);
}

void qmp_blockdev_add(BlockdevOptions *options, Error **errp)
{
    QmpOutputVisitor *ov = qmp_output_visitor_new();
    DriveInfo *dinfo;
    QObject *obj;
    QDict *qdict;
    Error *local_err = NULL;

    /* Require an ID in the top level */
    if (!options->has_id) {
        error_setg(errp, "Block device needs an ID");
        goto fail;
    }

    /* TODO Sort it out in raw-posix and drive_init: Reject aio=native with
     * cache.direct=false instead of silently switching to aio=threads, except
     * if called from drive_init.
     *
     * For now, simply forbidding the combination for all drivers will do. */
    if (options->has_aio && options->aio == BLOCKDEV_AIO_OPTIONS_NATIVE) {
        bool direct = options->has_cache &&
                      options->cache->has_direct &&
                      options->cache->direct;
        if (!direct) {
            error_setg(errp, "aio=native requires cache.direct=true");
            goto fail;
        }
    }

    visit_type_BlockdevOptions(qmp_output_get_visitor(ov),
                               &options, NULL, &local_err);
    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        goto fail;
    }

    obj = qmp_output_get_qobject(ov);
    qdict = qobject_to_qdict(obj);

    qdict_flatten(qdict);

    dinfo = blockdev_init(qdict, IF_NONE, &local_err);
    if (error_is_set(&local_err)) {
        error_propagate(errp, local_err);
        goto fail;
    }

    if (bdrv_key_required(dinfo->bdrv)) {
        drive_uninit(dinfo);
        error_setg(errp, "blockdev-add doesn't support encrypted devices");
        goto fail;
    }

fail:
    qmp_output_visitor_cleanup(ov);
}

static void do_qmp_query_block_jobs_one(void *opaque, BlockDriverState *bs)
{
    BlockJobInfoList **prev = opaque;
    BlockJob *job = bs->job;

    if (job) {
        BlockJobInfoList *elem = g_new0(BlockJobInfoList, 1);
        elem->value = block_job_query(bs->job);
        (*prev)->next = elem;
        *prev = elem;
    }
}

BlockJobInfoList *qmp_query_block_jobs(Error **errp)
{
    /* Dummy is a fake list element for holding the head pointer */
    BlockJobInfoList dummy = {};
    BlockJobInfoList *prev = &dummy;
    bdrv_iterate(do_qmp_query_block_jobs_one, &prev);
    return dummy.next;
}

QemuOptsList qemu_common_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_common_drive_opts.head),
    .desc = {
        {
            .name = "snapshot",
            .type = QEMU_OPT_BOOL,
            .help = "enable/disable snapshot mode",
        },{
            .name = "file",
            .type = QEMU_OPT_STRING,
            .help = "disk image",
        },{
            .name = "discard",
            .type = QEMU_OPT_STRING,
            .help = "discard operation (ignore/off, unmap/on)",
        },{
            .name = "cache.writeback",
            .type = QEMU_OPT_BOOL,
            .help = "enables writeback mode for any caches",
        },{
            .name = "cache.direct",
            .type = QEMU_OPT_BOOL,
            .help = "enables use of O_DIRECT (bypass the host page cache)",
        },{
            .name = "cache.no-flush",
            .type = QEMU_OPT_BOOL,
            .help = "ignore any flush requests for the device",
        },{
            .name = "aio",
            .type = QEMU_OPT_STRING,
            .help = "host AIO implementation (threads, native)",
        },{
            .name = "format",
            .type = QEMU_OPT_STRING,
            .help = "disk format (raw, qcow2, ...)",
        },{
            .name = "serial",
            .type = QEMU_OPT_STRING,
            .help = "disk serial number",
        },{
            .name = "rerror",
            .type = QEMU_OPT_STRING,
            .help = "read error action",
        },{
            .name = "werror",
            .type = QEMU_OPT_STRING,
            .help = "write error action",
        },{
            .name = "read-only",
            .type = QEMU_OPT_BOOL,
            .help = "open drive file as read-only",
        },{
            .name = "throttling.iops-total",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total I/O operations per second",
        },{
            .name = "throttling.iops-read",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read operations per second",
        },{
            .name = "throttling.iops-write",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write operations per second",
        },{
            .name = "throttling.bps-total",
            .type = QEMU_OPT_NUMBER,
            .help = "limit total bytes per second",
        },{
            .name = "throttling.bps-read",
            .type = QEMU_OPT_NUMBER,
            .help = "limit read bytes per second",
        },{
            .name = "throttling.bps-write",
            .type = QEMU_OPT_NUMBER,
            .help = "limit write bytes per second",
        },{
            .name = "copy-on-read",
            .type = QEMU_OPT_BOOL,
            .help = "copy read data from backing file into image file",
        },
        { /* end of list */ }
    },
};

QemuOptsList qemu_drive_opts = {
    .name = "drive",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_drive_opts.head),
    .desc = {
        /*
         * no elements => accept any params
         * validation will happen later
         */
        { /* end of list */ }
    },
};

QemuOptsList qemu_simple_drive_opts = {
    .name = "simple-drive",
    .implied_opt_name = "format",
    .head = QTAILQ_HEAD_INITIALIZER(qemu_simple_drive_opts.head),
    .desc = {
        /*
         * no elements => accept any
         * sanity checking will happen later
         * when setting device properties
         */
        { /* end if list */ }
    }
};
