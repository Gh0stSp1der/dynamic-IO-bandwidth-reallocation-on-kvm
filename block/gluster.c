/*
 * GlusterFS backend for QEMU
 *
 * Copyright (C) 2012 Bharata B Rao <bharata@linux.vnet.ibm.com>
 *
 * Pipe handling mechanism in AIO implementation is derived from
 * block/rbd.c. Hence,
 *
 * Copyright (C) 2010-2011 Christian Brunner <chb@muc.de>,
 *                         Josh Durgin <josh.durgin@dreamhost.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */
#include <glusterfs/api/glfs.h>
#include "block/block_int.h"
#include "qemu/sockets.h"
#include "qemu/uri.h"

typedef struct GlusterAIOCB {
    BlockDriverAIOCB common;
    int64_t size;
    int ret;
    bool *finished;
    QEMUBH *bh;
} GlusterAIOCB;

typedef struct BDRVGlusterState {
    struct glfs *glfs;
    int fds[2];
    struct glfs_fd *fd;
    int qemu_aio_count;
    int event_reader_pos;
    GlusterAIOCB *event_acb;
    int debug_level;
} BDRVGlusterState;

#define GLUSTER_FD_READ  0
#define GLUSTER_FD_WRITE 1

typedef struct GlusterConf {
    char *server;
    int port;
    char *volname;
    char *image;
    char *transport;
    int debug_level;
} GlusterConf;

static void qemu_gluster_gconf_free(GlusterConf *gconf)
{
    if (gconf) {
        g_free(gconf->server);
        g_free(gconf->volname);
        g_free(gconf->image);
        g_free(gconf->transport);
        g_free(gconf);
    }
}

static int parse_volume_options(GlusterConf *gconf, char *path)
{
    char *p, *q;

    if (!path) {
        return -EINVAL;
    }

    /* volume */
    p = q = path + strspn(path, "/");
    p += strcspn(p, "/");
    if (*p == '\0') {
        return -EINVAL;
    }
    gconf->volname = g_strndup(q, p - q);

    /* image */
    p += strspn(p, "/");
    if (*p == '\0') {
        return -EINVAL;
    }
    gconf->image = g_strdup(p);
    return 0;
}

/*
 * file=gluster[+transport]://[server[:port]]/volname/image[?socket=...]
 *
 * 'gluster' is the protocol.
 *
 * 'transport' specifies the transport type used to connect to gluster
 * management daemon (glusterd). Valid transport types are
 * tcp, unix and rdma. If a transport type isn't specified, then tcp
 * type is assumed.
 *
 * 'server' specifies the server where the volume file specification for
 * the given volume resides. This can be either hostname, ipv4 address
 * or ipv6 address. ipv6 address needs to be within square brackets [ ].
 * If transport type is 'unix', then 'server' field should not be specifed.
 * The 'socket' field needs to be populated with the path to unix domain
 * socket.
 *
 * 'port' is the port number on which glusterd is listening. This is optional
 * and if not specified, QEMU will send 0 which will make gluster to use the
 * default port. If the transport type is unix, then 'port' should not be
 * specified.
 *
 * 'volname' is the name of the gluster volume which contains the VM image.
 *
 * 'image' is the path to the actual VM image that resides on gluster volume.
 *
 * Examples:
 *
 * file=gluster://1.2.3.4/testvol/a.img
 * file=gluster+tcp://1.2.3.4/testvol/a.img
 * file=gluster+tcp://1.2.3.4:24007/testvol/dir/a.img
 * file=gluster+tcp://[1:2:3:4:5:6:7:8]/testvol/dir/a.img
 * file=gluster+tcp://[1:2:3:4:5:6:7:8]:24007/testvol/dir/a.img
 * file=gluster+tcp://server.domain.com:24007/testvol/dir/a.img
 * file=gluster+unix:///testvol/dir/a.img?socket=/tmp/glusterd.socket
 * file=gluster+rdma://1.2.3.4:24007/testvol/a.img
 */
static int qemu_gluster_parseuri(GlusterConf *gconf, const char *filename)
{
    URI *uri;
    QueryParams *qp = NULL;
    bool is_unix = false;
    int ret = 0;

    uri = uri_parse(filename);
    if (!uri) {
        return -EINVAL;
    }

    /* transport */
    if (!uri->scheme || !strcmp(uri->scheme, "gluster")) {
        gconf->transport = g_strdup("tcp");
    } else if (!strcmp(uri->scheme, "gluster+tcp")) {
        gconf->transport = g_strdup("tcp");
    } else if (!strcmp(uri->scheme, "gluster+unix")) {
        gconf->transport = g_strdup("unix");
        is_unix = true;
    } else if (!strcmp(uri->scheme, "gluster+rdma")) {
        gconf->transport = g_strdup("rdma");
    } else {
        ret = -EINVAL;
        goto out;
    }

    ret = parse_volume_options(gconf, uri->path);
    if (ret < 0) {
        goto out;
    }

    qp = query_params_parse(uri->query);
    if (qp->n > 1 || (is_unix && !qp->n) || (!is_unix && qp->n)) {
        ret = -EINVAL;
        goto out;
    }

    if (is_unix) {
        if (uri->server || uri->port) {
            ret = -EINVAL;
            goto out;
        }
        if (strcmp(qp->p[0].name, "socket")) {
            ret = -EINVAL;
            goto out;
        }
        gconf->server = g_strdup(qp->p[0].value);
    } else {
        gconf->server = g_strdup(uri->server ? uri->server : "localhost");
        gconf->port = uri->port;
    }

out:
    if (qp) {
        query_params_free(qp);
    }
    uri_free(uri);
    return ret;
}

static struct glfs *qemu_gluster_init(GlusterConf *gconf, const char *filename,
                                      Error **errp)
{
    struct glfs *glfs = NULL;
    int ret;
    int old_errno;

    ret = qemu_gluster_parseuri(gconf, filename);
    if (ret < 0) {
        error_setg(errp, "Usage: file=gluster[+transport]://[server[:port]]/"
                   "volname/image[?socket=...]");
        errno = -ret;
        goto out;
    }

    glfs = glfs_new(gconf->volname);
    if (!glfs) {
        goto out;
    }

    ret = glfs_set_volfile_server(glfs, gconf->transport, gconf->server,
            gconf->port);
    if (ret < 0) {
        goto out;
    }

    ret = glfs_set_logging(glfs, "-", gconf->debug_level);
    if (ret < 0) {
        goto out;
    }

    ret = glfs_init(glfs);
    if (ret) {
        error_setg_errno(errp, errno,
                         "Gluster connection failed for server=%s port=%d "
                         "volume=%s image=%s transport=%s", gconf->server,
                         gconf->port, gconf->volname, gconf->image,
                         gconf->transport);

        /* glfs_init sometimes doesn't set errno although docs suggest that */
        if (errno == 0)
            errno = EINVAL;

        goto out;
    }
    return glfs;

out:
    if (glfs) {
        old_errno = errno;
        glfs_fini(glfs);
        errno = old_errno;
    }
    return NULL;
}

static void qemu_gluster_complete_aio(GlusterAIOCB *acb, BDRVGlusterState *s)
{
    int ret;
    bool *finished = acb->finished;
    BlockDriverCompletionFunc *cb = acb->common.cb;
    void *opaque = acb->common.opaque;

    if (!acb->ret || acb->ret == acb->size) {
        ret = 0; /* Success */
    } else if (acb->ret < 0) {
        ret = acb->ret; /* Read/Write failed */
    } else {
        ret = -EIO; /* Partial read/write - fail it */
    }

    s->qemu_aio_count--;
    qemu_aio_release(acb);
    cb(opaque, ret);
    if (finished) {
        *finished = true;
    }
}

static void qemu_gluster_aio_event_reader(void *opaque)
{
    BDRVGlusterState *s = opaque;
    ssize_t ret;

    do {
        char *p = (char *)&s->event_acb;

        ret = read(s->fds[GLUSTER_FD_READ], p + s->event_reader_pos,
                   sizeof(s->event_acb) - s->event_reader_pos);
        if (ret > 0) {
            s->event_reader_pos += ret;
            if (s->event_reader_pos == sizeof(s->event_acb)) {
                s->event_reader_pos = 0;
                qemu_gluster_complete_aio(s->event_acb, s);
            }
        }
    } while (ret < 0 && errno == EINTR);
}

static int qemu_gluster_aio_flush_cb(void *opaque)
{
    BDRVGlusterState *s = opaque;

    return (s->qemu_aio_count > 0);
}

#define GLUSTER_OPT_FILENAME "filename"
#define GLUSTER_OPT_DEBUG "debug"
#define GLUSTER_DEBUG_DEFAULT 4
#define GLUSTER_DEBUG_MAX 9

/* TODO Convert to fine grained options */
static QemuOptsList runtime_opts = {
    .name = "gluster",
    .head = QTAILQ_HEAD_INITIALIZER(runtime_opts.head),
    .desc = {
        {
            .name = GLUSTER_OPT_FILENAME,
            .type = QEMU_OPT_STRING,
            .help = "URL to the gluster image",
        },
        {
            .name = GLUSTER_OPT_DEBUG,
            .type = QEMU_OPT_NUMBER,
            .help = "Gluster log level, valid range is 0-9",
        },
        { /* end of list */ }
    },
};

static void qemu_gluster_parse_flags(int bdrv_flags, int *open_flags)
{
    assert(open_flags != NULL);

    *open_flags |= O_BINARY;

    if (bdrv_flags & BDRV_O_RDWR) {
        *open_flags |= O_RDWR;
    } else {
        *open_flags |= O_RDONLY;
    }

    if ((bdrv_flags & BDRV_O_NOCACHE)) {
        *open_flags |= O_DIRECT;
    }
}

static int qemu_gluster_open(BlockDriverState *bs,  QDict *options,
                             int bdrv_flags, Error **errp)
{
    BDRVGlusterState *s = bs->opaque;
    int open_flags = 0;
    int ret = 0;
    GlusterConf *gconf = g_malloc0(sizeof(GlusterConf));
    QemuOpts *opts;
    Error *local_err = NULL;
    const char *filename;

    opts = qemu_opts_create_nofail(&runtime_opts);
    qemu_opts_absorb_qdict(opts, options, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        ret = -EINVAL;
        goto out;
    }

    filename = qemu_opt_get(opts, GLUSTER_OPT_FILENAME);

    s->debug_level = qemu_opt_get_number(opts, GLUSTER_OPT_DEBUG,
                                         GLUSTER_DEBUG_DEFAULT);
    if (s->debug_level < 0) {
        s->debug_level = 0;
    } else if (s->debug_level > GLUSTER_DEBUG_MAX) {
        s->debug_level = GLUSTER_DEBUG_MAX;
    }

    gconf->debug_level = s->debug_level;
    s->glfs = qemu_gluster_init(gconf, filename, errp);
    if (!s->glfs) {
        ret = -errno;
        goto out;
    }

    qemu_gluster_parse_flags(bdrv_flags, &open_flags);

    s->fd = glfs_open(s->glfs, gconf->image, open_flags);
    if (!s->fd) {
        ret = -errno;
        goto out;
    }

    ret = qemu_pipe(s->fds);
    if (ret < 0) {
        ret = -errno;
        goto out;
    }
    ret = fcntl(s->fds[GLUSTER_FD_READ], F_SETFL, O_NONBLOCK);
    if (ret < 0) {
        ret = -errno;
        close(s->fds[GLUSTER_FD_READ]);
        close(s->fds[GLUSTER_FD_WRITE]);
        goto out;
    }
    qemu_aio_set_fd_handler(s->fds[GLUSTER_FD_READ],
        qemu_gluster_aio_event_reader, NULL, qemu_gluster_aio_flush_cb, s);

out:
    qemu_opts_del(opts);
    qemu_gluster_gconf_free(gconf);
    if (!ret) {
        return ret;
    }
    if (s->fd) {
        glfs_close(s->fd);
    }
    if (s->glfs) {
        glfs_fini(s->glfs);
    }
    return ret;
}

typedef struct BDRVGlusterReopenState {
    struct glfs *glfs;
    struct glfs_fd *fd;
} BDRVGlusterReopenState;


static int qemu_gluster_reopen_prepare(BDRVReopenState *state,
                                       BlockReopenQueue *queue, Error **errp)
{
    int ret = 0;
    BDRVGlusterState *s;
    BDRVGlusterReopenState *reop_s;
    GlusterConf *gconf = NULL;
    int open_flags = 0;

    assert(state != NULL);
    assert(state->bs != NULL);

    s = state->bs->opaque;

    state->opaque = g_malloc0(sizeof(BDRVGlusterReopenState));
    reop_s = state->opaque;

    qemu_gluster_parse_flags(state->flags, &open_flags);

    gconf = g_malloc0(sizeof(GlusterConf));

    gconf->debug_level = s->debug_level;
    reop_s->glfs = qemu_gluster_init(gconf, state->bs->filename, errp);
    if (reop_s->glfs == NULL) {
        ret = -errno;
        goto exit;
    }

    reop_s->fd = glfs_open(reop_s->glfs, gconf->image, open_flags);
    if (reop_s->fd == NULL) {
        /* reops->glfs will be cleaned up in _abort */
        ret = -errno;
        goto exit;
    }

exit:
    /* state->opaque will be freed in either the _abort or _commit */
    qemu_gluster_gconf_free(gconf);
    return ret;
}

static void qemu_gluster_reopen_commit(BDRVReopenState *state)
{
    BDRVGlusterReopenState *reop_s = state->opaque;
    BDRVGlusterState *s = state->bs->opaque;


    /* close the old */
    if (s->fd) {
        glfs_close(s->fd);
    }
    if (s->glfs) {
        glfs_fini(s->glfs);
    }

    /* use the newly opened image / connection */
    s->fd         = reop_s->fd;
    s->glfs       = reop_s->glfs;

    g_free(state->opaque);
    state->opaque = NULL;

    return;
}


static void qemu_gluster_reopen_abort(BDRVReopenState *state)
{
    BDRVGlusterReopenState *reop_s = state->opaque;

    if (reop_s == NULL) {
        return;
    }

    if (reop_s->fd) {
        glfs_close(reop_s->fd);
    }

    if (reop_s->glfs) {
        glfs_fini(reop_s->glfs);
    }

    g_free(state->opaque);
    state->opaque = NULL;

    return;
}

static int qemu_gluster_create(const char *filename,
        QEMUOptionParameter *options, Error **errp)
{
    struct glfs *glfs;
    struct glfs_fd *fd;
    int ret = 0;
    int64_t total_size = 0;
    GlusterConf *gconf = g_malloc0(sizeof(GlusterConf));

    gconf->debug_level = GLUSTER_DEBUG_DEFAULT;
    while (options && options->name) {
        if (!strcmp(options->name, BLOCK_OPT_SIZE)) {
            total_size = options->value.n / BDRV_SECTOR_SIZE;
        }
        if (!strcmp(options->name, GLUSTER_OPT_DEBUG)) {
            gconf->debug_level = options->value.n;
            if (gconf->debug_level < 0) {
                gconf->debug_level = 0;
            } else if (gconf->debug_level > GLUSTER_DEBUG_MAX) {
                gconf->debug_level = GLUSTER_DEBUG_MAX;
            }
        }
        options++;
    }

    glfs = qemu_gluster_init(gconf, filename, errp);
    if (!glfs) {
        ret = -errno;
        goto out;
    }

    fd = glfs_creat(glfs, gconf->image,
        O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, S_IRUSR | S_IWUSR);
    if (!fd) {
        ret = -errno;
    } else {
        if (glfs_ftruncate(fd, total_size * BDRV_SECTOR_SIZE) != 0) {
            ret = -errno;
        }
        if (glfs_close(fd) != 0) {
            ret = -errno;
        }
    }
out:
    qemu_gluster_gconf_free(gconf);
    if (glfs) {
        glfs_fini(glfs);
    }
    return ret;
}

static void qemu_gluster_aio_cancel(BlockDriverAIOCB *blockacb)
{
    GlusterAIOCB *acb = (GlusterAIOCB *)blockacb;
    bool finished = false;

    acb->finished = &finished;
    while (!finished) {
        qemu_aio_wait();
    }
}

static const AIOCBInfo gluster_aiocb_info = {
    .aiocb_size = sizeof(GlusterAIOCB),
    .cancel = qemu_gluster_aio_cancel,
};

static void gluster_finish_aiocb(struct glfs_fd *fd, ssize_t ret, void *arg)
{
    GlusterAIOCB *acb = (GlusterAIOCB *)arg;
    BlockDriverState *bs = acb->common.bs;
    BDRVGlusterState *s = bs->opaque;
    int retval;

    acb->ret = ret;
    retval = qemu_write_full(s->fds[GLUSTER_FD_WRITE], &acb, sizeof(acb));
    if (retval != sizeof(acb)) {
        /*
         * Gluster AIO callback thread failed to notify the waiting
         * QEMU thread about IO completion.
         */
        error_report("Gluster AIO completion failed: %s", strerror(errno));
        abort();
    }
}

static BlockDriverAIOCB *qemu_gluster_aio_rw(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque, int write)
{
    int ret;
    GlusterAIOCB *acb;
    BDRVGlusterState *s = bs->opaque;
    size_t size;
    off_t offset;

    offset = sector_num * BDRV_SECTOR_SIZE;
    size = nb_sectors * BDRV_SECTOR_SIZE;
    s->qemu_aio_count++;

    acb = qemu_aio_get(&gluster_aiocb_info, bs, cb, opaque);
    acb->size = size;
    acb->ret = 0;
    acb->finished = NULL;

    if (write) {
        ret = glfs_pwritev_async(s->fd, qiov->iov, qiov->niov, offset, 0,
            &gluster_finish_aiocb, acb);
    } else {
        ret = glfs_preadv_async(s->fd, qiov->iov, qiov->niov, offset, 0,
            &gluster_finish_aiocb, acb);
    }

    if (ret < 0) {
        goto out;
    }
    return &acb->common;

out:
    s->qemu_aio_count--;
    qemu_aio_release(acb);
    return NULL;
}

static int qemu_gluster_truncate(BlockDriverState *bs, int64_t offset)
{
    int ret;
    BDRVGlusterState *s = bs->opaque;

    ret = glfs_ftruncate(s->fd, offset);
    if (ret < 0) {
        return -errno;
    }

    return 0;
}

static BlockDriverAIOCB *qemu_gluster_aio_readv(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    return qemu_gluster_aio_rw(bs, sector_num, qiov, nb_sectors, cb, opaque, 0);
}

static BlockDriverAIOCB *qemu_gluster_aio_writev(BlockDriverState *bs,
        int64_t sector_num, QEMUIOVector *qiov, int nb_sectors,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    return qemu_gluster_aio_rw(bs, sector_num, qiov, nb_sectors, cb, opaque, 1);
}

static BlockDriverAIOCB *qemu_gluster_aio_flush(BlockDriverState *bs,
        BlockDriverCompletionFunc *cb, void *opaque)
{
    int ret;
    GlusterAIOCB *acb;
    BDRVGlusterState *s = bs->opaque;

    acb = qemu_aio_get(&gluster_aiocb_info, bs, cb, opaque);
    acb->size = 0;
    acb->ret = 0;
    acb->finished = NULL;
    s->qemu_aio_count++;

    ret = glfs_fsync_async(s->fd, &gluster_finish_aiocb, acb);
    if (ret < 0) {
        goto out;
    }
    return &acb->common;

out:
    s->qemu_aio_count--;
    qemu_aio_release(acb);
    return NULL;
}

#ifdef CONFIG_GLUSTERFS_DISCARD
static BlockDriverAIOCB *qemu_gluster_aio_discard(BlockDriverState *bs,
        int64_t sector_num, int nb_sectors, BlockDriverCompletionFunc *cb,
        void *opaque)
{
    int ret;
    GlusterAIOCB *acb;
    BDRVGlusterState *s = bs->opaque;
    size_t size;
    off_t offset;

    offset = sector_num * BDRV_SECTOR_SIZE;
    size = nb_sectors * BDRV_SECTOR_SIZE;

    acb = qemu_aio_get(&gluster_aiocb_info, bs, cb, opaque);
    acb->size = 0;
    acb->ret = 0;
    acb->finished = NULL;
    s->qemu_aio_count++;

    ret = glfs_discard_async(s->fd, offset, size, &gluster_finish_aiocb, acb);
    if (ret < 0) {
        goto out;
    }
    return &acb->common;

out:
    s->qemu_aio_count--;
    qemu_aio_release(acb);
    return NULL;
}
#endif

static int64_t qemu_gluster_getlength(BlockDriverState *bs)
{
    BDRVGlusterState *s = bs->opaque;
    int64_t ret;

    ret = glfs_lseek(s->fd, 0, SEEK_END);
    if (ret < 0) {
        return -errno;
    } else {
        return ret;
    }
}

static int64_t qemu_gluster_allocated_file_size(BlockDriverState *bs)
{
    BDRVGlusterState *s = bs->opaque;
    struct stat st;
    int ret;

    ret = glfs_fstat(s->fd, &st);
    if (ret < 0) {
        return -errno;
    } else {
        return st.st_blocks * 512;
    }
}

static void qemu_gluster_close(BlockDriverState *bs)
{
    BDRVGlusterState *s = bs->opaque;

    close(s->fds[GLUSTER_FD_READ]);
    close(s->fds[GLUSTER_FD_WRITE]);
    qemu_aio_set_fd_handler(s->fds[GLUSTER_FD_READ], NULL, NULL, NULL, NULL);

    if (s->fd) {
        glfs_close(s->fd);
        s->fd = NULL;
    }
    glfs_fini(s->glfs);
}

static int qemu_gluster_has_zero_init(BlockDriverState *bs)
{
    /* GlusterFS volume could be backed by a block device */
    return 0;
}

static QEMUOptionParameter qemu_gluster_create_options[] = {
    {
        .name = BLOCK_OPT_SIZE,
        .type = OPT_SIZE,
        .help = "Virtual disk size"
    },
    {
        .name = GLUSTER_OPT_DEBUG,
        .type = QEMU_OPT_NUMBER,
        .help = "Gluster log level, valid range is 0-9",
    },
    { NULL }
};

static BlockDriver bdrv_gluster = {
    .format_name                  = "gluster",
    .protocol_name                = "gluster",
    .instance_size                = sizeof(BDRVGlusterState),
    .bdrv_file_open               = qemu_gluster_open,
    .bdrv_reopen_prepare          = qemu_gluster_reopen_prepare,
    .bdrv_reopen_commit           = qemu_gluster_reopen_commit,
    .bdrv_reopen_abort            = qemu_gluster_reopen_abort,
    .bdrv_close                   = qemu_gluster_close,
    .bdrv_create                  = qemu_gluster_create,
    .bdrv_getlength               = qemu_gluster_getlength,
    .bdrv_get_allocated_file_size = qemu_gluster_allocated_file_size,
    .bdrv_truncate                = qemu_gluster_truncate,
    .bdrv_aio_readv               = qemu_gluster_aio_readv,
    .bdrv_aio_writev              = qemu_gluster_aio_writev,
    .bdrv_aio_flush               = qemu_gluster_aio_flush,
    .bdrv_has_zero_init           = qemu_gluster_has_zero_init,
#ifdef CONFIG_GLUSTERFS_DISCARD
    .bdrv_aio_discard             = qemu_gluster_aio_discard,
#endif
    .create_options               = qemu_gluster_create_options,
};

static BlockDriver bdrv_gluster_tcp = {
    .format_name                  = "gluster",
    .protocol_name                = "gluster+tcp",
    .instance_size                = sizeof(BDRVGlusterState),
    .bdrv_file_open               = qemu_gluster_open,
    .bdrv_reopen_prepare          = qemu_gluster_reopen_prepare,
    .bdrv_reopen_commit           = qemu_gluster_reopen_commit,
    .bdrv_reopen_abort            = qemu_gluster_reopen_abort,
    .bdrv_close                   = qemu_gluster_close,
    .bdrv_create                  = qemu_gluster_create,
    .bdrv_getlength               = qemu_gluster_getlength,
    .bdrv_get_allocated_file_size = qemu_gluster_allocated_file_size,
    .bdrv_truncate                = qemu_gluster_truncate,
    .bdrv_aio_readv               = qemu_gluster_aio_readv,
    .bdrv_aio_writev              = qemu_gluster_aio_writev,
    .bdrv_aio_flush               = qemu_gluster_aio_flush,
    .bdrv_has_zero_init           = qemu_gluster_has_zero_init,
#ifdef CONFIG_GLUSTERFS_DISCARD
    .bdrv_aio_discard             = qemu_gluster_aio_discard,
#endif
    .create_options               = qemu_gluster_create_options,
};

static BlockDriver bdrv_gluster_unix = {
    .format_name                  = "gluster",
    .protocol_name                = "gluster+unix",
    .instance_size                = sizeof(BDRVGlusterState),
    .bdrv_file_open               = qemu_gluster_open,
    .bdrv_reopen_prepare          = qemu_gluster_reopen_prepare,
    .bdrv_reopen_commit           = qemu_gluster_reopen_commit,
    .bdrv_reopen_abort            = qemu_gluster_reopen_abort,
    .bdrv_close                   = qemu_gluster_close,
    .bdrv_create                  = qemu_gluster_create,
    .bdrv_getlength               = qemu_gluster_getlength,
    .bdrv_get_allocated_file_size = qemu_gluster_allocated_file_size,
    .bdrv_truncate                = qemu_gluster_truncate,
    .bdrv_aio_readv               = qemu_gluster_aio_readv,
    .bdrv_aio_writev              = qemu_gluster_aio_writev,
    .bdrv_aio_flush               = qemu_gluster_aio_flush,
    .bdrv_has_zero_init           = qemu_gluster_has_zero_init,
#ifdef CONFIG_GLUSTERFS_DISCARD
    .bdrv_aio_discard             = qemu_gluster_aio_discard,
#endif
    .create_options               = qemu_gluster_create_options,
};

static BlockDriver bdrv_gluster_rdma = {
    .format_name                  = "gluster",
    .protocol_name                = "gluster+rdma",
    .instance_size                = sizeof(BDRVGlusterState),
    .bdrv_file_open               = qemu_gluster_open,
    .bdrv_reopen_prepare          = qemu_gluster_reopen_prepare,
    .bdrv_reopen_commit           = qemu_gluster_reopen_commit,
    .bdrv_reopen_abort            = qemu_gluster_reopen_abort,
    .bdrv_close                   = qemu_gluster_close,
    .bdrv_create                  = qemu_gluster_create,
    .bdrv_getlength               = qemu_gluster_getlength,
    .bdrv_get_allocated_file_size = qemu_gluster_allocated_file_size,
    .bdrv_truncate                = qemu_gluster_truncate,
    .bdrv_aio_readv               = qemu_gluster_aio_readv,
    .bdrv_aio_writev              = qemu_gluster_aio_writev,
    .bdrv_aio_flush               = qemu_gluster_aio_flush,
    .bdrv_has_zero_init           = qemu_gluster_has_zero_init,
#ifdef CONFIG_GLUSTERFS_DISCARD
    .bdrv_aio_discard             = qemu_gluster_aio_discard,
#endif
    .create_options               = qemu_gluster_create_options,
};

static void bdrv_gluster_init(void)
{
    bdrv_register(&bdrv_gluster_rdma);
    bdrv_register(&bdrv_gluster_unix);
    bdrv_register(&bdrv_gluster_tcp);
    bdrv_register(&bdrv_gluster);
}

block_init(bdrv_gluster_init);
