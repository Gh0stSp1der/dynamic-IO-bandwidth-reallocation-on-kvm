/*
 * Virtio Block Device
 *
 * Copyright IBM, Corp. 2007
 *
 * Authors:
 *  Anthony Liguori   <aliguori@us.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include "qemu-common.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "hw/block/block.h"
#include "sysemu/blockdev.h"
#include "hw/virtio/virtio-blk.h"
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
# include "dataplane/virtio-blk.h"
# include "migration/migration.h"
#endif
#include "block/scsi.h"
#ifdef __linux__
# include <scsi/sg.h>
#endif
#include "hw/virtio/virtio-bus.h"

typedef struct VirtIOBlockReq
{
    VirtIOBlock *dev;
    VirtQueueElement elem;
    struct virtio_blk_inhdr *in;
    struct virtio_blk_outhdr *out;
    struct virtio_scsi_inhdr *scsi;
    QEMUIOVector qiov;
    struct VirtIOBlockReq *next;
    BlockAcctCookie acct;
} VirtIOBlockReq;

static void virtio_blk_req_complete(VirtIOBlockReq *req, int status)
{
    VirtIOBlock *s = req->dev;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);

    trace_virtio_blk_req_complete(req, status);

    stb_p(&req->in->status, status);
    virtqueue_push(s->vq, &req->elem, req->qiov.size + sizeof(*req->in));
    virtio_notify(vdev, s->vq);
}

static int virtio_blk_handle_rw_error(VirtIOBlockReq *req, int error,
    bool is_read)
{
    BlockErrorAction action = bdrv_get_error_action(req->dev->bs, is_read, error);
    VirtIOBlock *s = req->dev;

    if (action == BDRV_ACTION_STOP) {
        req->next = s->rq;
        s->rq = req;
    } else if (action == BDRV_ACTION_REPORT) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        bdrv_acct_done(s->bs, &req->acct);
        g_free(req);
    }

    bdrv_error_action(s->bs, action, is_read, error);
    return action != BDRV_ACTION_IGNORE;
}

static void virtio_blk_rw_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    trace_virtio_blk_rw_complete(req, ret);

    if (ret) {
        bool is_read = !(ldl_p(&req->out->type) & VIRTIO_BLK_T_OUT);
        if (virtio_blk_handle_rw_error(req, -ret, is_read))
            return;
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
    bdrv_acct_done(req->dev->bs, &req->acct);
    g_free(req);
}

static void virtio_blk_flush_complete(void *opaque, int ret)
{
    VirtIOBlockReq *req = opaque;

    if (ret) {
        if (virtio_blk_handle_rw_error(req, -ret, 0)) {
            return;
        }
    }

    virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
    bdrv_acct_done(req->dev->bs, &req->acct);
    g_free(req);
}

static VirtIOBlockReq *virtio_blk_alloc_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = g_malloc(sizeof(*req));
    req->dev = s;
    req->qiov.size = 0;
    req->next = NULL;
    return req;
}

static VirtIOBlockReq *virtio_blk_get_request(VirtIOBlock *s)
{
    VirtIOBlockReq *req = virtio_blk_alloc_request(s);

    if (req != NULL) {
        if (!virtqueue_pop(s->vq, &req->elem)) {
            g_free(req);
            return NULL;
        }
    }

    return req;
}

static void virtio_blk_handle_scsi(VirtIOBlockReq *req)
{
#ifdef __linux__
    int ret;
    int i;
#endif
    int status = VIRTIO_BLK_S_OK;

    /*
     * We require at least one output segment each for the virtio_blk_outhdr
     * and the SCSI command block.
     *
     * We also at least require the virtio_blk_inhdr, the virtio_scsi_inhdr
     * and the sense buffer pointer in the input segments.
     */
    if (req->elem.out_num < 2 || req->elem.in_num < 3) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        g_free(req);
        return;
    }

    /*
     * The scsi inhdr is placed in the second-to-last input segment, just
     * before the regular inhdr.
     */
    req->scsi = (void *)req->elem.in_sg[req->elem.in_num - 2].iov_base;

    if (!req->dev->blk.scsi) {
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }

    /*
     * No support for bidirection commands yet.
     */
    if (req->elem.out_num > 2 && req->elem.in_num > 3) {
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }

#ifdef __linux__
    struct sg_io_hdr hdr;
    memset(&hdr, 0, sizeof(struct sg_io_hdr));
    hdr.interface_id = 'S';
    hdr.cmd_len = req->elem.out_sg[1].iov_len;
    hdr.cmdp = req->elem.out_sg[1].iov_base;
    hdr.dxfer_len = 0;

    if (req->elem.out_num > 2) {
        /*
         * If there are more than the minimally required 2 output segments
         * there is write payload starting from the third iovec.
         */
        hdr.dxfer_direction = SG_DXFER_TO_DEV;
        hdr.iovec_count = req->elem.out_num - 2;

        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.out_sg[i + 2].iov_len;

        hdr.dxferp = req->elem.out_sg + 2;

    } else if (req->elem.in_num > 3) {
        /*
         * If we have more than 3 input segments the guest wants to actually
         * read data.
         */
        hdr.dxfer_direction = SG_DXFER_FROM_DEV;
        hdr.iovec_count = req->elem.in_num - 3;
        for (i = 0; i < hdr.iovec_count; i++)
            hdr.dxfer_len += req->elem.in_sg[i].iov_len;

        hdr.dxferp = req->elem.in_sg;
    } else {
        /*
         * Some SCSI commands don't actually transfer any data.
         */
        hdr.dxfer_direction = SG_DXFER_NONE;
    }

    hdr.sbp = req->elem.in_sg[req->elem.in_num - 3].iov_base;
    hdr.mx_sb_len = req->elem.in_sg[req->elem.in_num - 3].iov_len;

    ret = bdrv_ioctl(req->dev->bs, SG_IO, &hdr);
    if (ret) {
        status = VIRTIO_BLK_S_UNSUPP;
        goto fail;
    }

    /*
     * From SCSI-Generic-HOWTO: "Some lower level drivers (e.g. ide-scsi)
     * clear the masked_status field [hence status gets cleared too, see
     * block/scsi_ioctl.c] even when a CHECK_CONDITION or COMMAND_TERMINATED
     * status has occurred.  However they do set DRIVER_SENSE in driver_status
     * field. Also a (sb_len_wr > 0) indicates there is a sense buffer.
     */
    if (hdr.status == 0 && hdr.sb_len_wr > 0) {
        hdr.status = CHECK_CONDITION;
    }

    stl_p(&req->scsi->errors,
          hdr.status | (hdr.msg_status << 8) |
          (hdr.host_status << 16) | (hdr.driver_status << 24));
    stl_p(&req->scsi->residual, hdr.resid);
    stl_p(&req->scsi->sense_len, hdr.sb_len_wr);
    stl_p(&req->scsi->data_len, hdr.dxfer_len);

    virtio_blk_req_complete(req, status);
    g_free(req);
    return;
#else
    abort();
#endif

fail:
    /* Just put anything nonzero so that the ioctl fails in the guest.  */
    stl_p(&req->scsi->errors, 255);
    virtio_blk_req_complete(req, status);
    g_free(req);
}

typedef struct MultiReqBuffer {
    BlockRequest        blkreq[32];
    unsigned int        num_writes;
} MultiReqBuffer;

static void virtio_submit_multiwrite(BlockDriverState *bs, MultiReqBuffer *mrb)
{
    int i, ret;

    if (!mrb->num_writes) {
        return;
    }

    ret = bdrv_aio_multiwrite(bs, mrb->blkreq, mrb->num_writes);
    if (ret != 0) {
        for (i = 0; i < mrb->num_writes; i++) {
            if (mrb->blkreq[i].error) {
                virtio_blk_rw_complete(mrb->blkreq[i].opaque, -EIO);
            }
        }
    }

    mrb->num_writes = 0;
}

static void virtio_blk_handle_flush(VirtIOBlockReq *req, MultiReqBuffer *mrb)
{
    bdrv_acct_start(req->dev->bs, &req->acct, 0, BDRV_ACCT_FLUSH);

    /*
     * Make sure all outstanding writes are posted to the backing device.
     */
    virtio_submit_multiwrite(req->dev->bs, mrb);
    bdrv_aio_flush(req->dev->bs, virtio_blk_flush_complete, req);
}

static bool virtio_blk_sect_range_ok(VirtIOBlock *dev,
                                     uint64_t sector, size_t size)
{
    uint64_t nb_sectors = size >> BDRV_SECTOR_BITS;
    uint64_t total_sectors;

    if (sector & dev->sector_mask) {
        return false;
    }
    if (size % dev->conf->logical_block_size) {
        return false;
    }
    bdrv_get_geometry(dev->bs, &total_sectors);
    if (sector > total_sectors || nb_sectors > total_sectors - sector) {
        return false;
    }
    return true;
}

static void virtio_blk_handle_write(VirtIOBlockReq *req, MultiReqBuffer *mrb)
{
    BlockRequest *blkreq;
    uint64_t sector;

    sector = ldq_p(&req->out->sector);

    trace_virtio_blk_handle_write(req, sector, req->qiov.size / 512);

    if (!virtio_blk_sect_range_ok(req->dev, sector, req->qiov.size)) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        g_free(req);
        return;
    }

    bdrv_acct_start(req->dev->bs, &req->acct, req->qiov.size, BDRV_ACCT_WRITE);

    if (mrb->num_writes == 32) {
        virtio_submit_multiwrite(req->dev->bs, mrb);
    }

    blkreq = &mrb->blkreq[mrb->num_writes];
    blkreq->sector = sector;
    blkreq->nb_sectors = req->qiov.size / BDRV_SECTOR_SIZE;
    blkreq->qiov = &req->qiov;
    blkreq->cb = virtio_blk_rw_complete;
    blkreq->opaque = req;
    blkreq->error = 0;

    mrb->num_writes++;
}

static void virtio_blk_handle_read(VirtIOBlockReq *req)
{
    uint64_t sector;

    sector = ldq_p(&req->out->sector);

    trace_virtio_blk_handle_read(req, sector, req->qiov.size / 512);

    if (!virtio_blk_sect_range_ok(req->dev, sector, req->qiov.size)) {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_IOERR);
        g_free(req);
        return;
    }

    bdrv_acct_start(req->dev->bs, &req->acct, req->qiov.size, BDRV_ACCT_READ);
    bdrv_aio_readv(req->dev->bs, sector, &req->qiov,
                   req->qiov.size / BDRV_SECTOR_SIZE,
                   virtio_blk_rw_complete, req);
}

static void virtio_blk_handle_request(VirtIOBlockReq *req,
    MultiReqBuffer *mrb)
{
    uint32_t type;

    if (req->elem.out_num < 1 || req->elem.in_num < 1) {
        error_report("virtio-blk missing headers");
        exit(1);
    }

    if (req->elem.out_sg[0].iov_len < sizeof(*req->out) ||
        req->elem.in_sg[req->elem.in_num - 1].iov_len < sizeof(*req->in)) {
        error_report("virtio-blk header not in correct element");
        exit(1);
    }

    req->out = (void *)req->elem.out_sg[0].iov_base;
    req->in = (void *)req->elem.in_sg[req->elem.in_num - 1].iov_base;

    type = ldl_p(&req->out->type);

    if (type & VIRTIO_BLK_T_FLUSH) {
        virtio_blk_handle_flush(req, mrb);
    } else if (type & VIRTIO_BLK_T_SCSI_CMD) {
        virtio_blk_handle_scsi(req);
    } else if (type & VIRTIO_BLK_T_GET_ID) {
        VirtIOBlock *s = req->dev;

        /*
         * NB: per existing s/n string convention the string is
         * terminated by '\0' only when shorter than buffer.
         */
        strncpy(req->elem.in_sg[0].iov_base,
                s->blk.serial ? s->blk.serial : "",
                MIN(req->elem.in_sg[0].iov_len, VIRTIO_BLK_ID_BYTES));
        virtio_blk_req_complete(req, VIRTIO_BLK_S_OK);
        g_free(req);
    } else if (type & VIRTIO_BLK_T_OUT) {
        qemu_iovec_init_external(&req->qiov, &req->elem.out_sg[1],
                                 req->elem.out_num - 1);
        virtio_blk_handle_write(req, mrb);
    } else if (type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_BARRIER) {
        /* VIRTIO_BLK_T_IN is 0, so we can't just & it. */
        qemu_iovec_init_external(&req->qiov, &req->elem.in_sg[0],
                                 req->elem.in_num - 1);
        virtio_blk_handle_read(req);
    } else {
        virtio_blk_req_complete(req, VIRTIO_BLK_S_UNSUPP);
        g_free(req);
    }
}

static void virtio_blk_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    VirtIOBlockReq *req;
    MultiReqBuffer mrb = {
        .num_writes = 0,
    };

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    /* Some guests kick before setting VIRTIO_CONFIG_S_DRIVER_OK so start
     * dataplane here instead of waiting for .set_status().
     */
    if (s->dataplane) {
        virtio_blk_data_plane_start(s->dataplane);
        return;
    }
#endif

    while ((req = virtio_blk_get_request(s))) {
        virtio_blk_handle_request(req, &mrb);
    }

    virtio_submit_multiwrite(s->bs, &mrb);

    /*
     * FIXME: Want to check for completions before returning to guest mode,
     * so cached reads and writes are reported as quickly as possible. But
     * that should be done in the generic block layer.
     */
}

static void virtio_blk_dma_restart_bh(void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIOBlockReq *req = s->rq;
    MultiReqBuffer mrb = {
        .num_writes = 0,
    };

    qemu_bh_delete(s->bh);
    s->bh = NULL;

    s->rq = NULL;

    while (req) {
        virtio_blk_handle_request(req, &mrb);
        req = req->next;
    }

    virtio_submit_multiwrite(s->bs, &mrb);
}

static void virtio_blk_dma_restart_cb(void *opaque, int running,
                                      RunState state)
{
    VirtIOBlock *s = opaque;

    if (!running) {
        return;
    }

    if (!s->bh) {
        s->bh = qemu_bh_new(virtio_blk_dma_restart_bh, s);
        qemu_bh_schedule(s->bh);
    }
}

static void virtio_blk_reset(VirtIODevice *vdev)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    VirtIOBlockReq *req;

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    if (s->dataplane) {
        virtio_blk_data_plane_stop(s->dataplane);
    }
#endif

    /*
     * This should cancel pending requests, but can't do nicely until there
     * are per-device request lists.
     */
    bdrv_drain_all();
    bdrv_set_enable_write_cache(s->bs, s->original_wce);
    /* We drop queued requests after bdrv_drain_all() because bdrv_drain_all()
     * itself can produce them. */
    while (s->rq) {
        req = s->rq;
        s->rq = req->next;
        g_free(req);
    }
}

/* coalesce internal state, copy to pci i/o region 0
 */
static void virtio_blk_update_config(VirtIODevice *vdev, uint8_t *config)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    struct virtio_blk_config blkcfg;
    uint64_t capacity;
    int blk_size = s->conf->logical_block_size;

    bdrv_get_geometry(s->bs, &capacity);
    memset(&blkcfg, 0, sizeof(blkcfg));
    stq_p(&blkcfg.capacity, capacity);
    stl_p(&blkcfg.seg_max, 128 - 2);
    stw_p(&blkcfg.cylinders, s->conf->cyls);
    stl_p(&blkcfg.blk_size, blk_size);
    stw_p(&blkcfg.min_io_size, s->conf->min_io_size / blk_size);
    stw_p(&blkcfg.opt_io_size, s->conf->opt_io_size / blk_size);
    blkcfg.heads = s->conf->heads;
    /*
     * We must ensure that the block device capacity is a multiple of
     * the logical block size. If that is not the case, let's use
     * sector_mask to adopt the geometry to have a correct picture.
     * For those devices where the capacity is ok for the given geometry
     * we don't touch the sector value of the geometry, since some devices
     * (like s390 dasd) need a specific value. Here the capacity is already
     * cyls*heads*secs*blk_size and the sector value is not block size
     * divided by 512 - instead it is the amount of blk_size blocks
     * per track (cylinder).
     */
    if (bdrv_getlength(s->bs) /  s->conf->heads / s->conf->secs % blk_size) {
        blkcfg.sectors = s->conf->secs & ~s->sector_mask;
    } else {
        blkcfg.sectors = s->conf->secs;
    }
    blkcfg.size_max = 0;
    blkcfg.physical_block_exp = get_physical_block_exp(s->conf);
    blkcfg.alignment_offset = 0;
    blkcfg.wce = bdrv_enable_write_cache(s->bs);
    memcpy(config, &blkcfg, sizeof(struct virtio_blk_config));
}

static void virtio_blk_set_config(VirtIODevice *vdev, const uint8_t *config)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    struct virtio_blk_config blkcfg;

    memcpy(&blkcfg, config, sizeof(blkcfg));
    bdrv_set_enable_write_cache(s->bs, blkcfg.wce != 0);
}

static uint32_t virtio_blk_get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);

    features |= (1 << VIRTIO_BLK_F_SEG_MAX);
    features |= (1 << VIRTIO_BLK_F_GEOMETRY);
    features |= (1 << VIRTIO_BLK_F_TOPOLOGY);
    features |= (1 << VIRTIO_BLK_F_BLK_SIZE);
    features |= (1 << VIRTIO_BLK_F_SCSI);

    if (s->blk.config_wce) {
        features |= (1 << VIRTIO_BLK_F_CONFIG_WCE);
    }
    if (bdrv_enable_write_cache(s->bs))
        features |= (1 << VIRTIO_BLK_F_WCE);

    if (bdrv_is_read_only(s->bs))
        features |= 1 << VIRTIO_BLK_F_RO;

    return features;
}

static void virtio_blk_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    uint32_t features;

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    if (s->dataplane && !(status & (VIRTIO_CONFIG_S_DRIVER |
                                    VIRTIO_CONFIG_S_DRIVER_OK))) {
        virtio_blk_data_plane_stop(s->dataplane);
    }
#endif

    if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        return;
    }

    features = vdev->guest_features;

    /* A guest that supports VIRTIO_BLK_F_CONFIG_WCE must be able to send
     * cache flushes.  Thus, the "auto writethrough" behavior is never
     * necessary for guests that support the VIRTIO_BLK_F_CONFIG_WCE feature.
     * Leaving it enabled would break the following sequence:
     *
     *     Guest started with "-drive cache=writethrough"
     *     Guest sets status to 0
     *     Guest sets DRIVER bit in status field
     *     Guest reads host features (WCE=0, CONFIG_WCE=1)
     *     Guest writes guest features (WCE=0, CONFIG_WCE=1)
     *     Guest writes 1 to the WCE configuration field (writeback mode)
     *     Guest sets DRIVER_OK bit in status field
     *
     * s->bs would erroneously be placed in writethrough mode.
     */
    if (!(features & (1 << VIRTIO_BLK_F_CONFIG_WCE))) {
        bdrv_set_enable_write_cache(s->bs, !!(features & (1 << VIRTIO_BLK_F_WCE)));
    }
}

static void virtio_blk_save(QEMUFile *f, void *opaque)
{
    VirtIOBlock *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    VirtIOBlockReq *req = s->rq;

    virtio_save(vdev, f);
    
    while (req) {
        qemu_put_sbyte(f, 1);
        qemu_put_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req = req->next;
    }
    qemu_put_sbyte(f, 0);
}

static int virtio_blk_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOBlock *s = opaque;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    int ret;

    if (version_id != 2)
        return -EINVAL;

    ret = virtio_load(vdev, f);
    if (ret) {
        return ret;
    }

    while (qemu_get_sbyte(f)) {
        VirtIOBlockReq *req = virtio_blk_alloc_request(s);
        qemu_get_buffer(f, (unsigned char*)&req->elem, sizeof(req->elem));
        req->next = s->rq;
        s->rq = req;

        virtqueue_map_sg(req->elem.in_sg, req->elem.in_addr,
            req->elem.in_num, 1);
        virtqueue_map_sg(req->elem.out_sg, req->elem.out_addr,
            req->elem.out_num, 0);
    }

    return 0;
}

static void virtio_blk_resize(void *opaque)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(opaque);

    virtio_notify_config(vdev);
}

static const BlockDevOps virtio_block_ops = {
    .resize_cb = virtio_blk_resize,
};

void virtio_blk_set_conf(DeviceState *dev, VirtIOBlkConf *blk)
{
    VirtIOBlock *s = VIRTIO_BLK(dev);
    memcpy(&(s->blk), blk, sizeof(struct VirtIOBlkConf));
}

#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
/* Disable dataplane thread during live migration since it does not
 * update the dirty memory bitmap yet.
 */
static void virtio_blk_migration_state_changed(Notifier *notifier, void *data)
{
    VirtIOBlock *s = container_of(notifier, VirtIOBlock,
                                  migration_state_notifier);
    MigrationState *mig = data;

    if (migration_in_setup(mig)) {
        if (!s->dataplane) {
            return;
        }
        virtio_blk_data_plane_destroy(s->dataplane);
        s->dataplane = NULL;
    } else if (migration_has_finished(mig) ||
               migration_has_failed(mig)) {
        if (s->dataplane) {
            return;
        }
        bdrv_drain_all(); /* complete in-flight non-dataplane requests */
        virtio_blk_data_plane_create(VIRTIO_DEVICE(s), &s->blk,
                                     &s->dataplane);
    }
}
#endif /* CONFIG_VIRTIO_BLK_DATA_PLANE */

static int virtio_blk_device_init(VirtIODevice *vdev)
{
    DeviceState *qdev = DEVICE(vdev);
    VirtIOBlock *s = VIRTIO_BLK(vdev);
    VirtIOBlkConf *blk = &(s->blk);
    static int virtio_blk_id;

    if (!blk->conf.bs) {
        error_report("drive property not set");
        return -1;
    }
    if (!bdrv_is_inserted(blk->conf.bs)) {
        error_report("Device needs media, but drive is empty");
        return -1;
    }

    blkconf_serial(&blk->conf, &blk->serial);
    s->original_wce = bdrv_enable_write_cache(blk->conf.bs);
    if (blkconf_geometry(&blk->conf, NULL, 65535, 255, 255) < 0) {
        return -1;
    }

    virtio_init(vdev, "virtio-blk", VIRTIO_ID_BLOCK,
                sizeof(struct virtio_blk_config));

    s->bs = blk->conf.bs;
    s->conf = &blk->conf;
    memcpy(&(s->blk), blk, sizeof(struct VirtIOBlkConf));
    s->rq = NULL;
    s->sector_mask = (s->conf->logical_block_size / BDRV_SECTOR_SIZE) - 1;

    s->vq = virtio_add_queue(vdev, 128, virtio_blk_handle_output);
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    if (!virtio_blk_data_plane_create(vdev, blk, &s->dataplane)) {
        virtio_cleanup(vdev);
        return -1;
    }
    s->migration_state_notifier.notify = virtio_blk_migration_state_changed;
    add_migration_state_change_notifier(&s->migration_state_notifier);
#endif

    s->change = qemu_add_vm_change_state_handler(virtio_blk_dma_restart_cb, s);
    register_savevm(qdev, "virtio-blk", virtio_blk_id++, 2,
                    virtio_blk_save, virtio_blk_load, s);
    bdrv_set_dev_ops(s->bs, &virtio_block_ops, s);
    bdrv_set_guest_block_size(s->bs, s->conf->logical_block_size);

    bdrv_iostatus_enable(s->bs);

    add_boot_device_path(s->conf->bootindex, qdev, "/disk@0,0");
    return 0;
}

static void virtio_blk_device_exit(VirtIODevice *vdev)
{
    VirtIOBlock *s = VIRTIO_BLK(vdev);
#ifdef CONFIG_VIRTIO_BLK_DATA_PLANE
    remove_migration_state_change_notifier(&s->migration_state_notifier);
    virtio_blk_data_plane_destroy(s->dataplane);
    s->dataplane = NULL;
#endif
    qemu_del_vm_change_state_handler(s->change);
    unregister_savevm(DEVICE(vdev), "virtio-blk", s);
    blockdev_mark_auto_del(s->bs);
    virtio_cleanup(vdev);
}

static Property virtio_blk_properties[] = {
    DEFINE_VIRTIO_BLK_PROPERTIES(VirtIOBlock, blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void virtio_blk_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    dc->props = virtio_blk_properties;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->init = virtio_blk_device_init;
    vdc->exit = virtio_blk_device_exit;
    vdc->get_config = virtio_blk_update_config;
    vdc->set_config = virtio_blk_set_config;
    vdc->get_features = virtio_blk_get_features;
    vdc->set_status = virtio_blk_set_status;
    vdc->reset = virtio_blk_reset;
}

static const TypeInfo virtio_device_info = {
    .name = TYPE_VIRTIO_BLK,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOBlock),
    .class_init = virtio_blk_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_device_info);
}

type_init(virtio_register_types)
