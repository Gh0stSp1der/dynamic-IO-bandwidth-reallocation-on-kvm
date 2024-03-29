/*
 * vhost_scsi host device
 *
 * Copyright IBM, Corp. 2011
 *
 * Authors:
 *  Stefan Hajnoczi   <stefanha@linux.vnet.ibm.com>
 *
 * Changes for QEMU mainline + tcm_vhost kernel upstream:
 *  Nicholas Bellinger <nab@risingtidesystems.com>
 *
 * This work is licensed under the terms of the GNU LGPL, version 2 or later.
 * See the COPYING.LIB file in the top-level directory.
 *
 */

#include <sys/ioctl.h>
#include "config.h"
#include "qemu/queue.h"
#include "monitor/monitor.h"
#include "migration/migration.h"
#include "hw/virtio/vhost-scsi.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/virtio-scsi.h"
#include "hw/virtio/virtio-bus.h"

static int vhost_scsi_set_endpoint(VHostSCSI *s)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);
    struct vhost_scsi_target backend;
    int ret;

    memset(&backend, 0, sizeof(backend));
    pstrcpy(backend.vhost_wwpn, sizeof(backend.vhost_wwpn), vs->conf.wwpn);
    ret = ioctl(s->dev.control, VHOST_SCSI_SET_ENDPOINT, &backend);
    if (ret < 0) {
        return -errno;
    }
    return 0;
}

static void vhost_scsi_clear_endpoint(VHostSCSI *s)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(s);
    struct vhost_scsi_target backend;

    memset(&backend, 0, sizeof(backend));
    pstrcpy(backend.vhost_wwpn, sizeof(backend.vhost_wwpn), vs->conf.wwpn);
    ioctl(s->dev.control, VHOST_SCSI_CLEAR_ENDPOINT, &backend);
}

static int vhost_scsi_start(VHostSCSI *s)
{
    int ret, abi_version, i;
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return -ENOSYS;
    }

    ret = ioctl(s->dev.control, VHOST_SCSI_GET_ABI_VERSION, &abi_version);
    if (ret < 0) {
        return -errno;
    }
    if (abi_version > VHOST_SCSI_ABI_VERSION) {
        error_report("vhost-scsi: The running tcm_vhost kernel abi_version:"
                     " %d is greater than vhost_scsi userspace supports: %d, please"
                     " upgrade your version of QEMU\n", abi_version,
                     VHOST_SCSI_ABI_VERSION);
        return -ENOSYS;
    }

    ret = vhost_dev_enable_notifiers(&s->dev, vdev);
    if (ret < 0) {
        return ret;
    }

    s->dev.acked_features = vdev->guest_features;
    ret = vhost_dev_start(&s->dev, vdev);
    if (ret < 0) {
        error_report("Error start vhost dev");
        goto err_notifiers;
    }

    ret = vhost_scsi_set_endpoint(s);
    if (ret < 0) {
        error_report("Error set vhost-scsi endpoint");
        goto err_vhost_stop;
    }

    ret = k->set_guest_notifiers(qbus->parent, s->dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier");
        goto err_endpoint;
    }

    /* guest_notifier_mask/pending not used yet, so just unmask
     * everything here.  virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < s->dev.nvqs; i++) {
        vhost_virtqueue_mask(&s->dev, vdev, i, false);
    }

    return ret;

err_endpoint:
    vhost_scsi_clear_endpoint(s);
err_vhost_stop:
    vhost_dev_stop(&s->dev, vdev);
err_notifiers:
    vhost_dev_disable_notifiers(&s->dev, vdev);
    return ret;
}

static void vhost_scsi_stop(VHostSCSI *s)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(s);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret = 0;

    if (k->set_guest_notifiers) {
        ret = k->set_guest_notifiers(qbus->parent, s->dev.nvqs, false);
        if (ret < 0) {
                error_report("vhost guest notifier cleanup failed: %d\n", ret);
        }
    }
    assert(ret >= 0);

    vhost_scsi_clear_endpoint(s);
    vhost_dev_stop(&s->dev, vdev);
    vhost_dev_disable_notifiers(&s->dev, vdev);
}

static uint32_t vhost_scsi_get_features(VirtIODevice *vdev,
                                        uint32_t features)
{
    VHostSCSI *s = VHOST_SCSI(vdev);

    /* Clear features not supported by host kernel. */
    if (!(s->dev.features & (1 << VIRTIO_F_NOTIFY_ON_EMPTY))) {
        features &= ~(1 << VIRTIO_F_NOTIFY_ON_EMPTY);
    }
    if (!(s->dev.features & (1 << VIRTIO_RING_F_INDIRECT_DESC))) {
        features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
    }
    if (!(s->dev.features & (1 << VIRTIO_RING_F_EVENT_IDX))) {
        features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
    }
    if (!(s->dev.features & (1 << VIRTIO_SCSI_F_HOTPLUG))) {
        features &= ~(1 << VIRTIO_SCSI_F_HOTPLUG);
    }

    return features;
}

static void vhost_scsi_set_config(VirtIODevice *vdev,
                                  const uint8_t *config)
{
    VirtIOSCSIConfig *scsiconf = (VirtIOSCSIConfig *)config;
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(vdev);

    if ((uint32_t) ldl_p(&scsiconf->sense_size) != vs->sense_size ||
        (uint32_t) ldl_p(&scsiconf->cdb_size) != vs->cdb_size) {
        error_report("vhost-scsi does not support changing the sense data and CDB sizes");
        exit(1);
    }
}

static void vhost_scsi_set_status(VirtIODevice *vdev, uint8_t val)
{
    VHostSCSI *s = (VHostSCSI *)vdev;
    bool start = (val & VIRTIO_CONFIG_S_DRIVER_OK);

    if (s->dev.started == start) {
        return;
    }

    if (start) {
        int ret;

        ret = vhost_scsi_start(s);
        if (ret < 0) {
            error_report("virtio-scsi: unable to start vhost: %s\n",
                         strerror(-ret));

            /* There is no userspace virtio-scsi fallback so exit */
            exit(1);
        }
    } else {
        vhost_scsi_stop(s);
    }
}

static int vhost_scsi_init(VirtIODevice *vdev)
{
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(vdev);
    VHostSCSI *s = VHOST_SCSI(vdev);
    int vhostfd = -1;
    int ret;

    if (!vs->conf.wwpn) {
        error_report("vhost-scsi: missing wwpn\n");
        return -EINVAL;
    }

    if (vs->conf.vhostfd) {
        vhostfd = monitor_handle_fd_param(cur_mon, vs->conf.vhostfd);
        if (vhostfd == -1) {
            error_report("vhost-scsi: unable to parse vhostfd\n");
            return -EINVAL;
        }
    }

    ret = virtio_scsi_common_init(vs);
    if (ret < 0) {
        return ret;
    }

    s->dev.nvqs = VHOST_SCSI_VQ_NUM_FIXED + vs->conf.num_queues;
    s->dev.vqs = g_new(struct vhost_virtqueue, s->dev.nvqs);
    s->dev.vq_index = 0;

    ret = vhost_dev_init(&s->dev, vhostfd, "/dev/vhost-scsi", true);
    if (ret < 0) {
        error_report("vhost-scsi: vhost initialization failed: %s\n",
                strerror(-ret));
        return ret;
    }
    s->dev.backend_features = 0;

    error_setg(&s->migration_blocker,
            "vhost-scsi does not support migration");
    migrate_add_blocker(s->migration_blocker);

    return 0;
}

static void vhost_scsi_exit(VirtIODevice *vdev)
{
    VHostSCSI *s = VHOST_SCSI(vdev);
    VirtIOSCSICommon *vs = VIRTIO_SCSI_COMMON(vdev);

    migrate_del_blocker(s->migration_blocker);
    error_free(s->migration_blocker);

    /* This will stop vhost backend. */
    vhost_scsi_set_status(vdev, 0);

    g_free(s->dev.vqs);
    virtio_scsi_common_exit(vs);
}

static Property vhost_scsi_properties[] = {
    DEFINE_VHOST_SCSI_PROPERTIES(VHostSCSI, parent_obj.conf),
    DEFINE_PROP_END_OF_LIST(),
};

static void vhost_scsi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    dc->props = vhost_scsi_properties;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->init = vhost_scsi_init;
    vdc->exit = vhost_scsi_exit;
    vdc->get_features = vhost_scsi_get_features;
    vdc->set_config = vhost_scsi_set_config;
    vdc->set_status = vhost_scsi_set_status;
}

static const TypeInfo vhost_scsi_info = {
    .name = TYPE_VHOST_SCSI,
    .parent = TYPE_VIRTIO_SCSI_COMMON,
    .instance_size = sizeof(VHostSCSI),
    .class_init = vhost_scsi_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&vhost_scsi_info);
}

type_init(virtio_register_types)
