/*
 * Block driver for the QCOW version 2 format
 *
 * Copyright (c) 2004-2006 Fabrice Bellard
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

#include "qemu-common.h"
#include "block/block_int.h"
#include "block/qcow2.h"
#include "qemu/range.h"

static int64_t alloc_clusters_noref(BlockDriverState *bs, uint64_t size);
static int QEMU_WARN_UNUSED_RESULT update_refcount(BlockDriverState *bs,
                            int64_t offset, int64_t length,
                            int addend, enum qcow2_discard_type type);


/*********************************************************/
/* refcount handling */

int qcow2_refcount_init(BlockDriverState *bs)
{
    BDRVQcowState *s = bs->opaque;
    unsigned int refcount_table_size2, i;
    int ret;

    assert(s->refcount_table_size <= INT_MAX / sizeof(uint64_t));
    refcount_table_size2 = s->refcount_table_size * sizeof(uint64_t);
    s->refcount_table = g_malloc(refcount_table_size2);
    if (s->refcount_table_size > 0) {
        BLKDBG_EVENT(bs->file, BLKDBG_REFTABLE_LOAD);
        ret = bdrv_pread(bs->file, s->refcount_table_offset,
                         s->refcount_table, refcount_table_size2);
        if (ret != refcount_table_size2)
            goto fail;
        for(i = 0; i < s->refcount_table_size; i++)
            be64_to_cpus(&s->refcount_table[i]);
    }
    return 0;
 fail:
    return -ENOMEM;
}

void qcow2_refcount_close(BlockDriverState *bs)
{
    BDRVQcowState *s = bs->opaque;
    g_free(s->refcount_table);
}


static int load_refcount_block(BlockDriverState *bs,
                               int64_t refcount_block_offset,
                               void **refcount_block)
{
    BDRVQcowState *s = bs->opaque;
    int ret;

    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_LOAD);
    ret = qcow2_cache_get(bs, s->refcount_block_cache, refcount_block_offset,
        refcount_block);

    return ret;
}

/*
 * Returns the refcount of the cluster given by its index. Any non-negative
 * return value is the refcount of the cluster, negative values are -errno
 * and indicate an error.
 */
static int get_refcount(BlockDriverState *bs, int64_t cluster_index)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t refcount_table_index, block_index;
    int64_t refcount_block_offset;
    int ret;
    uint16_t *refcount_block;
    uint16_t refcount;

    refcount_table_index = cluster_index >> s->refcount_block_bits;
    if (refcount_table_index >= s->refcount_table_size)
        return 0;
    refcount_block_offset = s->refcount_table[refcount_table_index];
    if (!refcount_block_offset)
        return 0;

    if (offset_into_cluster(s, refcount_block_offset)) {
        qcow2_signal_corruption(bs, true, -1, -1, "Refblock offset %#" PRIx64
                                " unaligned (reftable index: %#" PRIx64 ")",
                                refcount_block_offset, refcount_table_index);
        return -EIO;
    }

    ret = qcow2_cache_get(bs, s->refcount_block_cache, refcount_block_offset,
        (void**) &refcount_block);
    if (ret < 0) {
        return ret;
    }

    block_index = cluster_index & (s->refcount_block_size - 1);
    refcount = be16_to_cpu(refcount_block[block_index]);

    ret = qcow2_cache_put(bs, s->refcount_block_cache,
        (void**) &refcount_block);
    if (ret < 0) {
        return ret;
    }

    return refcount;
}

/*
 * Rounds the refcount table size up to avoid growing the table for each single
 * refcount block that is allocated.
 */
static unsigned int next_refcount_table_size(BDRVQcowState *s,
    unsigned int min_size)
{
    unsigned int min_clusters = (min_size >> (s->cluster_bits - 3)) + 1;
    unsigned int refcount_table_clusters =
        MAX(1, s->refcount_table_size >> (s->cluster_bits - 3));

    while (min_clusters > refcount_table_clusters) {
        refcount_table_clusters = (refcount_table_clusters * 3 + 1) / 2;
    }

    return refcount_table_clusters << (s->cluster_bits - 3);
}


/* Checks if two offsets are described by the same refcount block */
static int in_same_refcount_block(BDRVQcowState *s, uint64_t offset_a,
    uint64_t offset_b)
{
    uint64_t block_a = offset_a >> (s->cluster_bits + s->refcount_block_bits);
    uint64_t block_b = offset_b >> (s->cluster_bits + s->refcount_block_bits);

    return (block_a == block_b);
}

/*
 * Loads a refcount block. If it doesn't exist yet, it is allocated first
 * (including growing the refcount table if needed).
 *
 * Returns 0 on success or -errno in error case
 */
static int alloc_refcount_block(BlockDriverState *bs,
    int64_t cluster_index, uint16_t **refcount_block)
{
    BDRVQcowState *s = bs->opaque;
    unsigned int refcount_table_index;
    int ret;

    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC);

    /* Find the refcount block for the given cluster */
    refcount_table_index = cluster_index >> s->refcount_block_bits;

    if (refcount_table_index < s->refcount_table_size) {

        uint64_t refcount_block_offset =
            s->refcount_table[refcount_table_index] & REFT_OFFSET_MASK;

        /* If it's already there, we're done */
        if (refcount_block_offset) {
            if (offset_into_cluster(s, refcount_block_offset)) {
                qcow2_signal_corruption(bs, true, -1, -1, "Refblock offset %#"
                                        PRIx64 " unaligned (reftable index: "
                                        "%#x)", refcount_block_offset,
                                        refcount_table_index);
                return -EIO;
            }

             return load_refcount_block(bs, refcount_block_offset,
                 (void**) refcount_block);
        }
    }

    /*
     * If we came here, we need to allocate something. Something is at least
     * a cluster for the new refcount block. It may also include a new refcount
     * table if the old refcount table is too small.
     *
     * Note that allocating clusters here needs some special care:
     *
     * - We can't use the normal qcow2_alloc_clusters(), it would try to
     *   increase the refcount and very likely we would end up with an endless
     *   recursion. Instead we must place the refcount blocks in a way that
     *   they can describe them themselves.
     *
     * - We need to consider that at this point we are inside update_refcounts
     *   and potentially doing an initial refcount increase. This means that
     *   some clusters have already been allocated by the caller, but their
     *   refcount isn't accurate yet. If we allocate clusters for metadata, we
     *   need to return -EAGAIN to signal the caller that it needs to restart
     *   the search for free clusters.
     *
     * - alloc_clusters_noref and qcow2_free_clusters may load a different
     *   refcount block into the cache
     */

    *refcount_block = NULL;

    /* We write to the refcount table, so we might depend on L2 tables */
    ret = qcow2_cache_flush(bs, s->l2_table_cache);
    if (ret < 0) {
        return ret;
    }

    /* Allocate the refcount block itself and mark it as used */
    int64_t new_block = alloc_clusters_noref(bs, s->cluster_size);
    if (new_block < 0) {
        return new_block;
    }

#ifdef DEBUG_ALLOC2
    fprintf(stderr, "qcow2: Allocate refcount block %d for %" PRIx64
        " at %" PRIx64 "\n",
        refcount_table_index, cluster_index << s->cluster_bits, new_block);
#endif

    if (in_same_refcount_block(s, new_block, cluster_index << s->cluster_bits)) {
        /* Zero the new refcount block before updating it */
        ret = qcow2_cache_get_empty(bs, s->refcount_block_cache, new_block,
            (void**) refcount_block);
        if (ret < 0) {
            goto fail_block;
        }

        memset(*refcount_block, 0, s->cluster_size);

        /* The block describes itself, need to update the cache */
        int block_index = (new_block >> s->cluster_bits) &
            (s->refcount_block_size - 1);
        (*refcount_block)[block_index] = cpu_to_be16(1);
    } else {
        /* Described somewhere else. This can recurse at most twice before we
         * arrive at a block that describes itself. */
        ret = update_refcount(bs, new_block, s->cluster_size, 1,
                              QCOW2_DISCARD_NEVER);
        if (ret < 0) {
            goto fail_block;
        }

        ret = qcow2_cache_flush(bs, s->refcount_block_cache);
        if (ret < 0) {
            goto fail_block;
        }

        /* Initialize the new refcount block only after updating its refcount,
         * update_refcount uses the refcount cache itself */
        ret = qcow2_cache_get_empty(bs, s->refcount_block_cache, new_block,
            (void**) refcount_block);
        if (ret < 0) {
            goto fail_block;
        }

        memset(*refcount_block, 0, s->cluster_size);
    }

    /* Now the new refcount block needs to be written to disk */
    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC_WRITE);
    qcow2_cache_entry_mark_dirty(s->refcount_block_cache, *refcount_block);
    ret = qcow2_cache_flush(bs, s->refcount_block_cache);
    if (ret < 0) {
        goto fail_block;
    }

    /* If the refcount table is big enough, just hook the block up there */
    if (refcount_table_index < s->refcount_table_size) {
        uint64_t data64 = cpu_to_be64(new_block);
        BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC_HOOKUP);
        ret = bdrv_pwrite_sync(bs->file,
            s->refcount_table_offset + refcount_table_index * sizeof(uint64_t),
            &data64, sizeof(data64));
        if (ret < 0) {
            goto fail_block;
        }

        s->refcount_table[refcount_table_index] = new_block;

        /* The new refcount block may be where the caller intended to put its
         * data, so let it restart the search. */
        return -EAGAIN;
    }

    ret = qcow2_cache_put(bs, s->refcount_block_cache, (void**) refcount_block);
    if (ret < 0) {
        goto fail_block;
    }

    /*
     * If we come here, we need to grow the refcount table. Again, a new
     * refcount table needs some space and we can't simply allocate to avoid
     * endless recursion.
     *
     * Therefore let's grab new refcount blocks at the end of the image, which
     * will describe themselves and the new refcount table. This way we can
     * reference them only in the new table and do the switch to the new
     * refcount table at once without producing an inconsistent state in
     * between.
     */
    BLKDBG_EVENT(bs->file, BLKDBG_REFTABLE_GROW);

    /* Calculate the number of refcount blocks needed so far; this will be the
     * basis for calculating the index of the first cluster used for the
     * self-describing refcount structures which we are about to create.
     *
     * Because we reached this point, there cannot be any refcount entries for
     * cluster_index or higher indices yet. However, because new_block has been
     * allocated to describe that cluster (and it will assume this role later
     * on), we cannot use that index; also, new_block may actually have a higher
     * cluster index than cluster_index, so it needs to be taken into account
     * here (and 1 needs to be added to its value because that cluster is used).
     */
    uint64_t blocks_used = DIV_ROUND_UP(MAX(cluster_index + 1,
                                            (new_block >> s->cluster_bits) + 1),
                                        s->refcount_block_size);

    if (blocks_used > QCOW_MAX_REFTABLE_SIZE / sizeof(uint64_t)) {
        return -EFBIG;
    }

    /* And now we need at least one block more for the new metadata */
    uint64_t table_size = next_refcount_table_size(s, blocks_used + 1);
    uint64_t last_table_size;
    uint64_t blocks_clusters;
    do {
        uint64_t table_clusters =
            size_to_clusters(s, table_size * sizeof(uint64_t));
        blocks_clusters = 1 +
            ((table_clusters + s->refcount_block_size - 1)
            / s->refcount_block_size);
        uint64_t meta_clusters = table_clusters + blocks_clusters;

        last_table_size = table_size;
        table_size = next_refcount_table_size(s, blocks_used +
            ((meta_clusters + s->refcount_block_size - 1)
            / s->refcount_block_size));

    } while (last_table_size != table_size);

#ifdef DEBUG_ALLOC2
    fprintf(stderr, "qcow2: Grow refcount table %" PRId32 " => %" PRId64 "\n",
        s->refcount_table_size, table_size);
#endif

    /* Create the new refcount table and blocks */
    uint64_t meta_offset = (blocks_used * s->refcount_block_size) *
        s->cluster_size;
    uint64_t table_offset = meta_offset + blocks_clusters * s->cluster_size;
    uint16_t *new_blocks = g_malloc0(blocks_clusters * s->cluster_size);
    uint64_t *new_table = g_malloc0(table_size * sizeof(uint64_t));

    /* Fill the new refcount table */
    memcpy(new_table, s->refcount_table,
        s->refcount_table_size * sizeof(uint64_t));
    new_table[refcount_table_index] = new_block;

    int i;
    for (i = 0; i < blocks_clusters; i++) {
        new_table[blocks_used + i] = meta_offset + (i * s->cluster_size);
    }

    /* Fill the refcount blocks */
    uint64_t table_clusters = size_to_clusters(s, table_size * sizeof(uint64_t));
    int block = 0;
    for (i = 0; i < table_clusters + blocks_clusters; i++) {
        new_blocks[block++] = cpu_to_be16(1);
    }

    /* Write refcount blocks to disk */
    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC_WRITE_BLOCKS);
    ret = bdrv_pwrite_sync(bs->file, meta_offset, new_blocks,
        blocks_clusters * s->cluster_size);
    g_free(new_blocks);
    if (ret < 0) {
        goto fail_table;
    }

    /* Write refcount table to disk */
    for(i = 0; i < table_size; i++) {
        cpu_to_be64s(&new_table[i]);
    }

    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC_WRITE_TABLE);
    ret = bdrv_pwrite_sync(bs->file, table_offset, new_table,
        table_size * sizeof(uint64_t));
    if (ret < 0) {
        goto fail_table;
    }

    for(i = 0; i < table_size; i++) {
        be64_to_cpus(&new_table[i]);
    }

    /* Hook up the new refcount table in the qcow2 header */
    uint8_t data[12];
    cpu_to_be64w((uint64_t*)data, table_offset);
    cpu_to_be32w((uint32_t*)(data + 8), table_clusters);
    BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_ALLOC_SWITCH_TABLE);
    ret = bdrv_pwrite_sync(bs->file, offsetof(QCowHeader, refcount_table_offset),
        data, sizeof(data));
    if (ret < 0) {
        goto fail_table;
    }

    /* And switch it in memory */
    uint64_t old_table_offset = s->refcount_table_offset;
    uint64_t old_table_size = s->refcount_table_size;

    g_free(s->refcount_table);
    s->refcount_table = new_table;
    s->refcount_table_size = table_size;
    s->refcount_table_offset = table_offset;

    /* Free old table. */
    qcow2_free_clusters(bs, old_table_offset, old_table_size * sizeof(uint64_t),
                        QCOW2_DISCARD_OTHER);

    ret = load_refcount_block(bs, new_block, (void**) refcount_block);
    if (ret < 0) {
        return ret;
    }

    /* If we were trying to do the initial refcount update for some cluster
     * allocation, we might have used the same clusters to store newly
     * allocated metadata. Make the caller search some new space. */
    return -EAGAIN;

fail_table:
    g_free(new_table);
fail_block:
    if (*refcount_block != NULL) {
        qcow2_cache_put(bs, s->refcount_block_cache, (void**) refcount_block);
    }
    return ret;
}

void qcow2_process_discards(BlockDriverState *bs, int ret)
{
    BDRVQcowState *s = bs->opaque;
    Qcow2DiscardRegion *d, *next;

    QTAILQ_FOREACH_SAFE(d, &s->discards, next, next) {
        QTAILQ_REMOVE(&s->discards, d, next);

        /* Discard is optional, ignore the return value */
        if (ret >= 0) {
            bdrv_discard(bs->file,
                         d->offset >> BDRV_SECTOR_BITS,
                         d->bytes >> BDRV_SECTOR_BITS);
        }

        g_free(d);
    }
}

static void update_refcount_discard(BlockDriverState *bs,
                                    uint64_t offset, uint64_t length)
{
    BDRVQcowState *s = bs->opaque;
    Qcow2DiscardRegion *d, *p, *next;

    QTAILQ_FOREACH(d, &s->discards, next) {
        uint64_t new_start = MIN(offset, d->offset);
        uint64_t new_end = MAX(offset + length, d->offset + d->bytes);

        if (new_end - new_start <= length + d->bytes) {
            /* There can't be any overlap, areas ending up here have no
             * references any more and therefore shouldn't get freed another
             * time. */
            assert(d->bytes + length == new_end - new_start);
            d->offset = new_start;
            d->bytes = new_end - new_start;
            goto found;
        }
    }

    d = g_malloc(sizeof(*d));
    *d = (Qcow2DiscardRegion) {
        .bs     = bs,
        .offset = offset,
        .bytes  = length,
    };
    QTAILQ_INSERT_TAIL(&s->discards, d, next);

found:
    /* Merge discard requests if they are adjacent now */
    QTAILQ_FOREACH_SAFE(p, &s->discards, next, next) {
        if (p == d
            || p->offset > d->offset + d->bytes
            || d->offset > p->offset + p->bytes)
        {
            continue;
        }

        /* Still no overlap possible */
        assert(p->offset == d->offset + d->bytes
            || d->offset == p->offset + p->bytes);

        QTAILQ_REMOVE(&s->discards, p, next);
        d->offset = MIN(d->offset, p->offset);
        d->bytes += p->bytes;
        g_free(p);
    }
}

/* XXX: cache several refcount block clusters ? */
static int QEMU_WARN_UNUSED_RESULT update_refcount(BlockDriverState *bs,
    int64_t offset, int64_t length, int addend, enum qcow2_discard_type type)
{
    BDRVQcowState *s = bs->opaque;
    int64_t start, last, cluster_offset;
    uint16_t *refcount_block = NULL;
    int64_t old_table_index = -1;
    int ret;

#ifdef DEBUG_ALLOC2
    fprintf(stderr, "update_refcount: offset=%" PRId64 " size=%" PRId64 " addend=%d\n",
           offset, length, addend);
#endif
    if (length < 0) {
        return -EINVAL;
    } else if (length == 0) {
        return 0;
    }

    if (addend < 0) {
        qcow2_cache_set_dependency(bs, s->refcount_block_cache,
            s->l2_table_cache);
    }

    start = offset & ~(s->cluster_size - 1);
    last = (offset + length - 1) & ~(s->cluster_size - 1);
    for(cluster_offset = start; cluster_offset <= last;
        cluster_offset += s->cluster_size)
    {
        int block_index, refcount;
        int64_t cluster_index = cluster_offset >> s->cluster_bits;
        int64_t table_index = cluster_index >> s->refcount_block_bits;

        /* Load the refcount block and allocate it if needed */
        if (table_index != old_table_index) {
            if (refcount_block) {
                ret = qcow2_cache_put(bs, s->refcount_block_cache,
                    (void**) &refcount_block);
                if (ret < 0) {
                    goto fail;
                }
            }

            ret = alloc_refcount_block(bs, cluster_index, &refcount_block);
            if (ret < 0) {
                goto fail;
            }
        }
        old_table_index = table_index;

        qcow2_cache_entry_mark_dirty(s->refcount_block_cache, refcount_block);

        /* we can update the count and save it */
        block_index = cluster_index & (s->refcount_block_size - 1);

        refcount = be16_to_cpu(refcount_block[block_index]);
        refcount += addend;
        if (refcount < 0 || refcount > 0xffff) {
            ret = -EINVAL;
            goto fail;
        }
        if (refcount == 0 && cluster_index < s->free_cluster_index) {
            s->free_cluster_index = cluster_index;
        }
        refcount_block[block_index] = cpu_to_be16(refcount);

        if (refcount == 0 && s->discard_passthrough[type]) {
            update_refcount_discard(bs, cluster_offset, s->cluster_size);
        }
    }

    ret = 0;
fail:
    if (!s->cache_discards) {
        qcow2_process_discards(bs, ret);
    }

    /* Write last changed block to disk */
    if (refcount_block) {
        int wret;
        wret = qcow2_cache_put(bs, s->refcount_block_cache,
            (void**) &refcount_block);
        if (wret < 0) {
            return ret < 0 ? ret : wret;
        }
    }

    /*
     * Try do undo any updates if an error is returned (This may succeed in
     * some cases like ENOSPC for allocating a new refcount block)
     */
    if (ret < 0) {
        int dummy;
        dummy = update_refcount(bs, offset, cluster_offset - offset, -addend,
                                QCOW2_DISCARD_NEVER);
        (void)dummy;
    }

    return ret;
}

/*
 * Increases or decreases the refcount of a given cluster by one.
 * addend must be 1 or -1.
 *
 * If the return value is non-negative, it is the new refcount of the cluster.
 * If it is negative, it is -errno and indicates an error.
 */
int qcow2_update_cluster_refcount(BlockDriverState *bs,
                                  int64_t cluster_index,
                                  int addend,
                                  enum qcow2_discard_type type)
{
    BDRVQcowState *s = bs->opaque;
    int ret;

    ret = update_refcount(bs, cluster_index << s->cluster_bits, 1, addend,
                          type);
    if (ret < 0) {
        return ret;
    }

    return get_refcount(bs, cluster_index);
}



/*********************************************************/
/* cluster allocation functions */



/* return < 0 if error */
static int64_t alloc_clusters_noref(BlockDriverState *bs, uint64_t size)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t i, nb_clusters;
    int refcount;

    /* We can't allocate clusters if they may still be queued for discard. */
    if (s->cache_discards) {
        qcow2_process_discards(bs, 0);
    }

    nb_clusters = size_to_clusters(s, size);
retry:
    for(i = 0; i < nb_clusters; i++) {
        uint64_t next_cluster_index = s->free_cluster_index++;
        refcount = get_refcount(bs, next_cluster_index);

        if (refcount < 0) {
            return refcount;
        } else if (refcount != 0) {
            goto retry;
        }
    }
#ifdef DEBUG_ALLOC2
    fprintf(stderr, "alloc_clusters: size=%" PRId64 " -> %" PRId64 "\n",
            size,
            (s->free_cluster_index - nb_clusters) << s->cluster_bits);
#endif
    return (s->free_cluster_index - nb_clusters) << s->cluster_bits;
}

int64_t qcow2_alloc_clusters(BlockDriverState *bs, uint64_t size)
{
    int64_t offset;
    int ret;

    BLKDBG_EVENT(bs->file, BLKDBG_CLUSTER_ALLOC);
    do {
        offset = alloc_clusters_noref(bs, size);
        if (offset < 0) {
            return offset;
        }

        ret = update_refcount(bs, offset, size, 1, QCOW2_DISCARD_NEVER);
    } while (ret == -EAGAIN);

    if (ret < 0) {
        return ret;
    }

    return offset;
}

int qcow2_alloc_clusters_at(BlockDriverState *bs, uint64_t offset,
    int nb_clusters)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t cluster_index;
    uint64_t i;
    int refcount, ret;

    assert(nb_clusters >= 0);
    if (nb_clusters == 0) {
        return 0;
    }

    do {
        /* Check how many clusters there are free */
        cluster_index = offset >> s->cluster_bits;
        for(i = 0; i < nb_clusters; i++) {
            refcount = get_refcount(bs, cluster_index++);

            if (refcount < 0) {
                return refcount;
            } else if (refcount != 0) {
                break;
            }
        }

        /* And then allocate them */
        ret = update_refcount(bs, offset, i << s->cluster_bits, 1,
                              QCOW2_DISCARD_NEVER);
    } while (ret == -EAGAIN);

    if (ret < 0) {
        return ret;
    }

    return i;
}

/* only used to allocate compressed sectors. We try to allocate
   contiguous sectors. size must be <= cluster_size */
int64_t qcow2_alloc_bytes(BlockDriverState *bs, int size)
{
    BDRVQcowState *s = bs->opaque;
    int64_t offset, cluster_offset;
    int free_in_cluster;

    BLKDBG_EVENT(bs->file, BLKDBG_CLUSTER_ALLOC_BYTES);
    assert(size > 0 && size <= s->cluster_size);
    if (s->free_byte_offset == 0) {
        offset = qcow2_alloc_clusters(bs, s->cluster_size);
        if (offset < 0) {
            return offset;
        }
        s->free_byte_offset = offset;
    }
 redo:
    free_in_cluster = s->cluster_size -
        (s->free_byte_offset & (s->cluster_size - 1));
    if (size <= free_in_cluster) {
        /* enough space in current cluster */
        offset = s->free_byte_offset;
        s->free_byte_offset += size;
        free_in_cluster -= size;
        if (free_in_cluster == 0)
            s->free_byte_offset = 0;
        if ((offset & (s->cluster_size - 1)) != 0)
            qcow2_update_cluster_refcount(bs, offset >> s->cluster_bits, 1,
                                          QCOW2_DISCARD_NEVER);
    } else {
        offset = qcow2_alloc_clusters(bs, s->cluster_size);
        if (offset < 0) {
            return offset;
        }
        cluster_offset = s->free_byte_offset & ~(s->cluster_size - 1);
        if ((cluster_offset + s->cluster_size) == offset) {
            /* we are lucky: contiguous data */
            offset = s->free_byte_offset;
            qcow2_update_cluster_refcount(bs, offset >> s->cluster_bits, 1,
                                          QCOW2_DISCARD_NEVER);
            s->free_byte_offset += size;
        } else {
            s->free_byte_offset = offset;
            goto redo;
        }
    }

    /* The cluster refcount was incremented, either by qcow2_alloc_clusters()
     * or explicitly by qcow2_update_cluster_refcount().  Refcount blocks must
     * be flushed before the caller's L2 table updates.
     */
    qcow2_cache_set_dependency(bs, s->l2_table_cache, s->refcount_block_cache);
    return offset;
}

void qcow2_free_clusters(BlockDriverState *bs,
                          int64_t offset, int64_t size,
                          enum qcow2_discard_type type)
{
    int ret;

    BLKDBG_EVENT(bs->file, BLKDBG_CLUSTER_FREE);
    ret = update_refcount(bs, offset, size, -1, type);
    if (ret < 0) {
        fprintf(stderr, "qcow2_free_clusters failed: %s\n", strerror(-ret));
        /* TODO Remember the clusters to free them later and avoid leaking */
    }
}

/*
 * Free a cluster using its L2 entry (handles clusters of all types, e.g.
 * normal cluster, compressed cluster, etc.)
 */
void qcow2_free_any_clusters(BlockDriverState *bs, uint64_t l2_entry,
                             int nb_clusters, enum qcow2_discard_type type)
{
    BDRVQcowState *s = bs->opaque;

    switch (qcow2_get_cluster_type(l2_entry)) {
    case QCOW2_CLUSTER_COMPRESSED:
        {
            int nb_csectors;
            nb_csectors = ((l2_entry >> s->csize_shift) &
                           s->csize_mask) + 1;
            qcow2_free_clusters(bs,
                (l2_entry & s->cluster_offset_mask) & ~511,
                nb_csectors * 512, type);
        }
        break;
    case QCOW2_CLUSTER_NORMAL:
    case QCOW2_CLUSTER_ZERO:
        if (l2_entry & L2E_OFFSET_MASK) {
            if (offset_into_cluster(s, l2_entry & L2E_OFFSET_MASK)) {
                qcow2_signal_corruption(bs, false, -1, -1,
                                        "Cannot free unaligned cluster %#llx",
                                        l2_entry & L2E_OFFSET_MASK);
            } else {
                qcow2_free_clusters(bs, l2_entry & L2E_OFFSET_MASK,
                                    nb_clusters << s->cluster_bits, type);
            }
        }
        break;
    case QCOW2_CLUSTER_UNALLOCATED:
        break;
    default:
        abort();
    }
}



/*********************************************************/
/* snapshots and image creation */



/* update the refcounts of snapshots and the copied flag */
int qcow2_update_snapshot_refcount(BlockDriverState *bs,
    int64_t l1_table_offset, int l1_size, int addend)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t *l1_table, *l2_table, l2_offset, offset, l1_size2, l1_allocated;
    int64_t old_offset, old_l2_offset;
    int i, j, l1_modified = 0, nb_csectors, refcount;
    int ret;

    l2_table = NULL;
    l1_table = NULL;
    l1_size2 = l1_size * sizeof(uint64_t);

    s->cache_discards = true;

    /* WARNING: qcow2_snapshot_goto relies on this function not using the
     * l1_table_offset when it is the current s->l1_table_offset! Be careful
     * when changing this! */
    if (l1_table_offset != s->l1_table_offset) {
        l1_table = g_malloc0(align_offset(l1_size2, 512));
        l1_allocated = 1;

        ret = bdrv_pread(bs->file, l1_table_offset, l1_table, l1_size2);
        if (ret < 0) {
            goto fail;
        }

        for(i = 0;i < l1_size; i++)
            be64_to_cpus(&l1_table[i]);
    } else {
        assert(l1_size == s->l1_size);
        l1_table = s->l1_table;
        l1_allocated = 0;
    }

    for(i = 0; i < l1_size; i++) {
        l2_offset = l1_table[i];
        if (l2_offset) {
            old_l2_offset = l2_offset;
            l2_offset &= L1E_OFFSET_MASK;

            if (offset_into_cluster(s, l2_offset)) {
                qcow2_signal_corruption(bs, true, -1, -1, "L2 table offset %#"
                                        PRIx64 " unaligned (L1 index: %#x)",
                                        l2_offset, i);
                ret = -EIO;
                goto fail;
            }

            ret = qcow2_cache_get(bs, s->l2_table_cache, l2_offset,
                (void**) &l2_table);
            if (ret < 0) {
                goto fail;
            }

            for(j = 0; j < s->l2_size; j++) {
                uint64_t cluster_index;

                offset = be64_to_cpu(l2_table[j]);
                old_offset = offset;
                offset &= ~QCOW_OFLAG_COPIED;

                switch (qcow2_get_cluster_type(offset)) {
                    case QCOW2_CLUSTER_COMPRESSED:
                        nb_csectors = ((offset >> s->csize_shift) &
                                       s->csize_mask) + 1;
                        if (addend != 0) {
                            int ret;
                            ret = update_refcount(bs,
                                (offset & s->cluster_offset_mask) & ~511,
                                nb_csectors * 512, addend,
                                QCOW2_DISCARD_SNAPSHOT);
                            if (ret < 0) {
                                goto fail;
                            }
                        }
                        /* compressed clusters are never modified */
                        refcount = 2;
                        break;

                    case QCOW2_CLUSTER_NORMAL:
                    case QCOW2_CLUSTER_ZERO:
                        if (offset_into_cluster(s, offset & L2E_OFFSET_MASK)) {
                            qcow2_signal_corruption(bs, true, -1, -1, "Data "
                                                    "cluster offset %#llx "
                                                    "unaligned (L2 offset: %#"
                                                    PRIx64 ", L2 index: %#x)",
                                                    offset & L2E_OFFSET_MASK,
                                                    l2_offset, j);
                            ret = -EIO;
                            goto fail;
                        }

                        cluster_index = (offset & L2E_OFFSET_MASK) >> s->cluster_bits;
                        if (!cluster_index) {
                            /* unallocated */
                            refcount = 0;
                            break;
                        }
                        if (addend != 0) {
                            refcount = qcow2_update_cluster_refcount(bs,
                                    cluster_index, addend,
                                    QCOW2_DISCARD_SNAPSHOT);
                        } else {
                            refcount = get_refcount(bs, cluster_index);
                        }

                        if (refcount < 0) {
                            ret = refcount;
                            goto fail;
                        }
                        break;

                    case QCOW2_CLUSTER_UNALLOCATED:
                        refcount = 0;
                        break;

                    default:
                        abort();
                }

                if (refcount == 1) {
                    offset |= QCOW_OFLAG_COPIED;
                }
                if (offset != old_offset) {
                    if (addend > 0) {
                        qcow2_cache_set_dependency(bs, s->l2_table_cache,
                            s->refcount_block_cache);
                    }
                    l2_table[j] = cpu_to_be64(offset);
                    qcow2_cache_entry_mark_dirty(s->l2_table_cache, l2_table);
                }
            }

            ret = qcow2_cache_put(bs, s->l2_table_cache, (void**) &l2_table);
            if (ret < 0) {
                goto fail;
            }


            if (addend != 0) {
                refcount = qcow2_update_cluster_refcount(bs, l2_offset >>
                        s->cluster_bits, addend, QCOW2_DISCARD_SNAPSHOT);
            } else {
                refcount = get_refcount(bs, l2_offset >> s->cluster_bits);
            }
            if (refcount < 0) {
                ret = refcount;
                goto fail;
            } else if (refcount == 1) {
                l2_offset |= QCOW_OFLAG_COPIED;
            }
            if (l2_offset != old_l2_offset) {
                l1_table[i] = l2_offset;
                l1_modified = 1;
            }
        }
    }

    ret = bdrv_flush(bs);
fail:
    if (l2_table) {
        qcow2_cache_put(bs, s->l2_table_cache, (void**) &l2_table);
    }

    s->cache_discards = false;
    qcow2_process_discards(bs, ret);

    /* Update L1 only if it isn't deleted anyway (addend = -1) */
    if (ret == 0 && addend >= 0 && l1_modified) {
        for (i = 0; i < l1_size; i++) {
            cpu_to_be64s(&l1_table[i]);
        }

        ret = bdrv_pwrite_sync(bs->file, l1_table_offset, l1_table, l1_size2);

        for (i = 0; i < l1_size; i++) {
            be64_to_cpus(&l1_table[i]);
        }
    }
    if (l1_allocated)
        g_free(l1_table);
    return ret;
}




/*********************************************************/
/* refcount checking functions */



/*
 * Increases the refcount for a range of clusters in a given refcount table.
 * This is used to construct a temporary refcount table out of L1 and L2 tables
 * which can be compared the the refcount table saved in the image.
 *
 * Modifies the number of errors in res.
 */
static int inc_refcounts(BlockDriverState *bs,
                         BdrvCheckResult *res,
                         uint16_t **refcount_table,
                         int64_t *refcount_table_size,
                         int64_t offset, int64_t size)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t start, last, cluster_offset, k;

    if (size <= 0) {
        return 0;
    }

    start = offset & ~(s->cluster_size - 1);
    last = (offset + size - 1) & ~(s->cluster_size - 1);
    for(cluster_offset = start; cluster_offset <= last;
        cluster_offset += s->cluster_size) {
        k = cluster_offset >> s->cluster_bits;
        if (k >= *refcount_table_size) {
            int64_t old_refcount_table_size = *refcount_table_size;
            uint16_t *new_refcount_table;

            *refcount_table_size = k + 1;
            new_refcount_table = g_try_realloc(*refcount_table,
                                               *refcount_table_size *
                                               sizeof(**refcount_table));
            if (!new_refcount_table) {
                *refcount_table_size = old_refcount_table_size;
                res->check_errors++;
                return -ENOMEM;
            }
            *refcount_table = new_refcount_table;

            memset(*refcount_table + old_refcount_table_size, 0,
                   (*refcount_table_size - old_refcount_table_size) *
                   sizeof(**refcount_table));
        }

        if (++(*refcount_table)[k] == 0) {
            fprintf(stderr, "ERROR: overflow cluster offset=0x%" PRIx64
                    "\n", cluster_offset);
            res->corruptions++;
        }
    }

    return 0;
}

/* Flags for check_refcounts_l1() and check_refcounts_l2() */
enum {
    CHECK_FRAG_INFO = 0x2,      /* update BlockFragInfo counters */
};

/*
 * Increases the refcount in the given refcount table for the all clusters
 * referenced in the L2 table. While doing so, performs some checks on L2
 * entries.
 *
 * Returns the number of errors found by the checks or -errno if an internal
 * error occurred.
 */
static int check_refcounts_l2(BlockDriverState *bs, BdrvCheckResult *res,
    uint16_t **refcount_table, int64_t *refcount_table_size, int64_t l2_offset,
    int flags)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t *l2_table, l2_entry;
    uint64_t next_contiguous_offset = 0;
    int i, l2_size, nb_csectors, ret;

    /* Read L2 table from disk */
    l2_size = s->l2_size * sizeof(uint64_t);
    l2_table = g_malloc(l2_size);

    ret = bdrv_pread(bs->file, l2_offset, l2_table, l2_size);
    if (ret < 0) {
        fprintf(stderr, "ERROR: I/O error in check_refcounts_l2\n");
        res->check_errors++;
        goto fail;
    }

    /* Do the actual checks */
    for(i = 0; i < s->l2_size; i++) {
        l2_entry = be64_to_cpu(l2_table[i]);

        switch (qcow2_get_cluster_type(l2_entry)) {
        case QCOW2_CLUSTER_COMPRESSED:
            /* Compressed clusters don't have QCOW_OFLAG_COPIED */
            if (l2_entry & QCOW_OFLAG_COPIED) {
                fprintf(stderr, "ERROR: cluster %" PRId64 ": "
                    "copied flag must never be set for compressed "
                    "clusters\n", l2_entry >> s->cluster_bits);
                l2_entry &= ~QCOW_OFLAG_COPIED;
                res->corruptions++;
            }

            /* Mark cluster as used */
            nb_csectors = ((l2_entry >> s->csize_shift) &
                           s->csize_mask) + 1;
            l2_entry &= s->cluster_offset_mask;
            ret = inc_refcounts(bs, res, refcount_table, refcount_table_size,
                                l2_entry & ~511, nb_csectors * 512);
            if (ret < 0) {
                goto fail;
            }

            if (flags & CHECK_FRAG_INFO) {
                res->bfi.allocated_clusters++;
                res->bfi.compressed_clusters++;

                /* Compressed clusters are fragmented by nature.  Since they
                 * take up sub-sector space but we only have sector granularity
                 * I/O we need to re-read the same sectors even for adjacent
                 * compressed clusters.
                 */
                res->bfi.fragmented_clusters++;
            }
            break;

        case QCOW2_CLUSTER_ZERO:
            if ((l2_entry & L2E_OFFSET_MASK) == 0) {
                break;
            }
            /* fall through */

        case QCOW2_CLUSTER_NORMAL:
        {
            uint64_t offset = l2_entry & L2E_OFFSET_MASK;

            if (flags & CHECK_FRAG_INFO) {
                res->bfi.allocated_clusters++;
                if (next_contiguous_offset &&
                    offset != next_contiguous_offset) {
                    res->bfi.fragmented_clusters++;
                }
                next_contiguous_offset = offset + s->cluster_size;
            }

            /* Mark cluster as used */
            ret = inc_refcounts(bs, res, refcount_table, refcount_table_size,
                                offset, s->cluster_size);
            if (ret < 0) {
                goto fail;
            }

            /* Correct offsets are cluster aligned */
            if (offset & (s->cluster_size - 1)) {
                fprintf(stderr, "ERROR offset=%" PRIx64 ": Cluster is not "
                    "properly aligned; L2 entry corrupted.\n", offset);
                res->corruptions++;
            }
            break;
        }

        case QCOW2_CLUSTER_UNALLOCATED:
            break;

        default:
            abort();
        }
    }

    g_free(l2_table);
    return 0;

fail:
    g_free(l2_table);
    return ret;
}

/*
 * Increases the refcount for the L1 table, its L2 tables and all referenced
 * clusters in the given refcount table. While doing so, performs some checks
 * on L1 and L2 entries.
 *
 * Returns the number of errors found by the checks or -errno if an internal
 * error occurred.
 */
static int check_refcounts_l1(BlockDriverState *bs,
                              BdrvCheckResult *res,
                              uint16_t **refcount_table,
                              int64_t *refcount_table_size,
                              int64_t l1_table_offset, int l1_size,
                              int flags)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t *l1_table = NULL, l2_offset, l1_size2;
    int i, ret;

    l1_size2 = l1_size * sizeof(uint64_t);

    /* Mark L1 table as used */
    ret = inc_refcounts(bs, res, refcount_table, refcount_table_size,
                        l1_table_offset, l1_size2);
    if (ret < 0) {
        goto fail;
    }

    /* Read L1 table entries from disk */
    if (l1_size2 > 0) {
        l1_table = g_try_malloc(l1_size2);
        if (l1_table == NULL) {
            ret = -ENOMEM;
            res->check_errors++;
            goto fail;
        }
        ret = bdrv_pread(bs->file, l1_table_offset, l1_table, l1_size2);
        if (ret < 0) {
            fprintf(stderr, "ERROR: I/O error in check_refcounts_l1\n");
            res->check_errors++;
            goto fail;
        }
        for(i = 0;i < l1_size; i++)
            be64_to_cpus(&l1_table[i]);
    }

    /* Do the actual checks */
    for(i = 0; i < l1_size; i++) {
        l2_offset = l1_table[i];
        if (l2_offset) {
            /* Mark L2 table as used */
            l2_offset &= L1E_OFFSET_MASK;
            ret = inc_refcounts(bs, res, refcount_table, refcount_table_size,
                                l2_offset, s->cluster_size);
            if (ret < 0) {
                goto fail;
            }

            /* L2 tables are cluster aligned */
            if (l2_offset & (s->cluster_size - 1)) {
                fprintf(stderr, "ERROR l2_offset=%" PRIx64 ": Table is not "
                    "cluster aligned; L1 entry corrupted\n", l2_offset);
                res->corruptions++;
            }

            /* Process and check L2 entries */
            ret = check_refcounts_l2(bs, res, refcount_table,
                                     refcount_table_size, l2_offset, flags);
            if (ret < 0) {
                goto fail;
            }
        }
    }
    g_free(l1_table);
    return 0;

fail:
    g_free(l1_table);
    return ret;
}

/*
 * Checks the OFLAG_COPIED flag for all L1 and L2 entries.
 *
 * This function does not print an error message nor does it increment
 * check_errors if get_refcount fails (this is because such an error will have
 * been already detected and sufficiently signaled by the calling function
 * (qcow2_check_refcounts) by the time this function is called).
 */
static int check_oflag_copied(BlockDriverState *bs, BdrvCheckResult *res,
                              BdrvCheckMode fix)
{
    BDRVQcowState *s = bs->opaque;
    uint64_t *l2_table = qemu_blockalign(bs, s->cluster_size);
    int ret;
    int refcount;
    int i, j;
    bool repair;

    if (fix & BDRV_FIX_ERRORS) {
        /* Always repair */
        repair = true;
    } else if (fix & BDRV_FIX_LEAKS) {
        /* Repair only if that seems safe: This function is always
         * called after the refcounts have been fixed, so the refcount
         * is accurate if that repair was successful */
        repair = !res->check_errors && !res->corruptions && !res->leaks;
    } else {
        repair = false;
    }

    for (i = 0; i < s->l1_size; i++) {
        uint64_t l1_entry = s->l1_table[i];
        uint64_t l2_offset = l1_entry & L1E_OFFSET_MASK;
        bool l2_dirty = false;

        if (!l2_offset) {
            continue;
        }

        refcount = get_refcount(bs, l2_offset >> s->cluster_bits);
        if (refcount < 0) {
            /* don't print message nor increment check_errors */
            continue;
        }
        if ((refcount == 1) != ((l1_entry & QCOW_OFLAG_COPIED) != 0)) {
            fprintf(stderr, "%s OFLAG_COPIED L2 cluster: l1_index=%d "
                    "l1_entry=%" PRIx64 " refcount=%d\n",
                    repair ? "Repairing" : "ERROR", i, l1_entry, refcount);
            if (repair) {
                s->l1_table[i] = refcount == 1
                               ? l1_entry |  QCOW_OFLAG_COPIED
                               : l1_entry & ~QCOW_OFLAG_COPIED;
                ret = qcow2_write_l1_entry(bs, i);
                if (ret < 0) {
                    res->check_errors++;
                    goto fail;
                }
                res->corruptions_fixed++;
            } else {
                res->corruptions++;
            }
        }

        ret = bdrv_pread(bs->file, l2_offset, l2_table,
                         s->l2_size * sizeof(uint64_t));
        if (ret < 0) {
            fprintf(stderr, "ERROR: Could not read L2 table: %s\n",
                    strerror(-ret));
            res->check_errors++;
            goto fail;
        }

        for (j = 0; j < s->l2_size; j++) {
            uint64_t l2_entry = be64_to_cpu(l2_table[j]);
            uint64_t data_offset = l2_entry & L2E_OFFSET_MASK;
            int cluster_type = qcow2_get_cluster_type(l2_entry);

            if ((cluster_type == QCOW2_CLUSTER_NORMAL) ||
                ((cluster_type == QCOW2_CLUSTER_ZERO) && (data_offset != 0))) {
                refcount = get_refcount(bs, data_offset >> s->cluster_bits);
                if (refcount < 0) {
                    /* don't print message nor increment check_errors */
                    continue;
                }
                if ((refcount == 1) != ((l2_entry & QCOW_OFLAG_COPIED) != 0)) {
                    fprintf(stderr, "%s OFLAG_COPIED data cluster: "
                            "l2_entry=%" PRIx64 " refcount=%d\n",
                            repair ? "Repairing" : "ERROR", l2_entry, refcount);
                    if (repair) {
                        l2_table[j] = cpu_to_be64(refcount == 1
                                    ? l2_entry |  QCOW_OFLAG_COPIED
                                    : l2_entry & ~QCOW_OFLAG_COPIED);
                        l2_dirty = true;
                        res->corruptions_fixed++;
                    } else {
                        res->corruptions++;
                    }
                }
            }
        }

        if (l2_dirty) {
            ret = qcow2_pre_write_overlap_check(bs, QCOW2_OL_ACTIVE_L2,
                                                l2_offset, s->cluster_size);
            if (ret < 0) {
                fprintf(stderr, "ERROR: Could not write L2 table; metadata "
                        "overlap check failed: %s\n", strerror(-ret));
                res->check_errors++;
                goto fail;
            }

            ret = bdrv_pwrite(bs->file, l2_offset, l2_table, s->cluster_size);
            if (ret < 0) {
                fprintf(stderr, "ERROR: Could not write L2 table: %s\n",
                        strerror(-ret));
                res->check_errors++;
                goto fail;
            }
        }
    }

    ret = 0;

fail:
    qemu_vfree(l2_table);
    return ret;
}

/*
 * Checks consistency of refblocks and accounts for each refblock in
 * *refcount_table.
 */
static int check_refblocks(BlockDriverState *bs, BdrvCheckResult *res,
                           BdrvCheckMode fix, bool *rebuild,
                           uint16_t **refcount_table, int64_t *nb_clusters)
{
    BDRVQcowState *s = bs->opaque;
    int64_t i, size;
    int ret;

    for(i = 0; i < s->refcount_table_size; i++) {
        uint64_t offset, cluster;
        offset = s->refcount_table[i];
        cluster = offset >> s->cluster_bits;

        /* Refcount blocks are cluster aligned */
        if (offset & (s->cluster_size - 1)) {
            fprintf(stderr, "ERROR refcount block %" PRId64 " is not "
                "cluster aligned; refcount table entry corrupted\n", i);
            res->corruptions++;
            *rebuild = true;
            continue;
        }

        if (cluster >= *nb_clusters) {
            fprintf(stderr, "%s refcount block %" PRId64 " is outside image\n",
                    fix & BDRV_FIX_ERRORS ? "Repairing" : "ERROR", i);

            if (fix & BDRV_FIX_ERRORS) {
                int64_t old_nb_clusters = *nb_clusters;
                uint16_t *new_refcount_table;

                if (offset > INT64_MAX - s->cluster_size) {
                    ret = -EINVAL;
                    goto resize_fail;
                }

                ret = bdrv_truncate(bs->file, offset + s->cluster_size);
                if (ret < 0) {
                    goto resize_fail;
                }
                size = bdrv_getlength(bs->file);
                if (size < 0) {
                    ret = size;
                    goto resize_fail;
                }

                *nb_clusters = size_to_clusters(s, size);
                assert(*nb_clusters >= old_nb_clusters);

                new_refcount_table = g_try_realloc(*refcount_table,
                                                   *nb_clusters *
                                                   sizeof(**refcount_table));
                if (!new_refcount_table) {
                    *nb_clusters = old_nb_clusters;
                    res->check_errors++;
                    return -ENOMEM;
                }
                *refcount_table = new_refcount_table;

                memset(*refcount_table + old_nb_clusters, 0,
                       (*nb_clusters - old_nb_clusters) *
                       sizeof(**refcount_table));

                if (cluster >= *nb_clusters) {
                    ret = -EINVAL;
                    goto resize_fail;
                }

                res->corruptions_fixed++;
                ret = inc_refcounts(bs, res, refcount_table, nb_clusters,
                                    offset, s->cluster_size);
                if (ret < 0) {
                    return ret;
                }
                /* No need to check whether the refcount is now greater than 1:
                 * This area was just allocated and zeroed, so it can only be
                 * exactly 1 after inc_refcounts() */
                continue;

resize_fail:
                res->corruptions++;
                *rebuild = true;
                fprintf(stderr, "ERROR could not resize image: %s\n",
                        strerror(-ret));
            } else {
                res->corruptions++;
            }
            continue;
        }

        if (offset != 0) {
            ret = inc_refcounts(bs, res, refcount_table, nb_clusters,
                                offset, s->cluster_size);
            if (ret < 0) {
                return ret;
            }
            if ((*refcount_table)[cluster] != 1) {
                fprintf(stderr, "ERROR refcount block %" PRId64
                        " refcount=%d\n", i, (*refcount_table)[cluster]);
                res->corruptions++;
                *rebuild = true;
            }
        }
    }

    return 0;
}

/*
 * Calculates an in-memory refcount table.
 */
static int calculate_refcounts(BlockDriverState *bs, BdrvCheckResult *res,
                               BdrvCheckMode fix, bool *rebuild,
                               uint16_t **refcount_table, int64_t *nb_clusters)
{
    BDRVQcowState *s = bs->opaque;
    int64_t i;
    QCowSnapshot *sn;
    int ret;

    if (!*refcount_table) {
        *refcount_table = g_try_new0(uint16_t, *nb_clusters);
        if (*nb_clusters && *refcount_table == NULL) {
            res->check_errors++;
            return -ENOMEM;
        }
    }

    /* header */
    ret = inc_refcounts(bs, res, refcount_table, nb_clusters,
                        0, s->cluster_size);
    if (ret < 0) {
        return ret;
    }

    /* current L1 table */
    ret = check_refcounts_l1(bs, res, refcount_table, nb_clusters,
                             s->l1_table_offset, s->l1_size, CHECK_FRAG_INFO);
    if (ret < 0) {
        return ret;
    }

    /* snapshots */
    for (i = 0; i < s->nb_snapshots; i++) {
        sn = s->snapshots + i;
        ret = check_refcounts_l1(bs, res, refcount_table, nb_clusters,
                                 sn->l1_table_offset, sn->l1_size, 0);
        if (ret < 0) {
            return ret;
        }
    }
    ret = inc_refcounts(bs, res, refcount_table, nb_clusters,
                        s->snapshots_offset, s->snapshots_size);
    if (ret < 0) {
        return ret;
    }

    /* refcount data */
    ret = inc_refcounts(bs, res, refcount_table, nb_clusters,
                        s->refcount_table_offset,
                        s->refcount_table_size * sizeof(uint64_t));
    if (ret < 0) {
        return ret;
    }

    return check_refblocks(bs, res, fix, rebuild, refcount_table, nb_clusters);
}

/*
 * Compares the actual reference count for each cluster in the image against the
 * refcount as reported by the refcount structures on-disk.
 */
static void compare_refcounts(BlockDriverState *bs, BdrvCheckResult *res,
                              BdrvCheckMode fix, bool *rebuild,
                              int64_t *highest_cluster,
                              uint16_t *refcount_table, int64_t nb_clusters)
{
    BDRVQcowState *s = bs->opaque;
    int64_t i;
    int refcount1, refcount2, ret;

    for (i = 0, *highest_cluster = 0; i < nb_clusters; i++) {
        refcount1 = get_refcount(bs, i);
        if (refcount1 < 0) {
            fprintf(stderr, "Can't get refcount for cluster %" PRId64 ": %s\n",
                i, strerror(-refcount1));
            res->check_errors++;
            continue;
        }

        refcount2 = refcount_table[i];

        if (refcount1 > 0 || refcount2 > 0) {
            *highest_cluster = i;
        }

        if (refcount1 != refcount2) {
            /* Check if we're allowed to fix the mismatch */
            int *num_fixed = NULL;
            if (refcount1 == 0) {
                *rebuild = true;
            } else if (refcount1 > refcount2 && (fix & BDRV_FIX_LEAKS)) {
                num_fixed = &res->leaks_fixed;
            } else if (refcount1 < refcount2 && (fix & BDRV_FIX_ERRORS)) {
                num_fixed = &res->corruptions_fixed;
            }

            fprintf(stderr, "%s cluster %" PRId64 " refcount=%d reference=%d\n",
                   num_fixed != NULL     ? "Repairing" :
                   refcount1 < refcount2 ? "ERROR" :
                                           "Leaked",
                   i, refcount1, refcount2);

            if (num_fixed) {
                ret = update_refcount(bs, i << s->cluster_bits, 1,
                                      refcount2 - refcount1,
                                      QCOW2_DISCARD_ALWAYS);
                if (ret >= 0) {
                    (*num_fixed)++;
                    continue;
                }
            }

            /* And if we couldn't, print an error */
            if (refcount1 < refcount2) {
                res->corruptions++;
            } else {
                res->leaks++;
            }
        }
    }
}

/*
 * Allocates clusters using an in-memory refcount table (IMRT) in contrast to
 * the on-disk refcount structures.
 *
 * On input, *first_free_cluster tells where to start looking, and need not
 * actually be a free cluster; the returned offset will not be before that
 * cluster.  On output, *first_free_cluster points to the first gap found, even
 * if that gap was too small to be used as the returned offset.
 *
 * Note that *first_free_cluster is a cluster index whereas the return value is
 * an offset.
 */
static int64_t alloc_clusters_imrt(BlockDriverState *bs,
                                   int cluster_count,
                                   uint16_t **refcount_table,
                                   int64_t *imrt_nb_clusters,
                                   int64_t *first_free_cluster)
{
    BDRVQcowState *s = bs->opaque;
    int64_t cluster = *first_free_cluster, i;
    bool first_gap = true;
    int contiguous_free_clusters;

    /* Starting at *first_free_cluster, find a range of at least cluster_count
     * continuously free clusters */
    for (contiguous_free_clusters = 0;
         cluster < *imrt_nb_clusters &&
         contiguous_free_clusters < cluster_count;
         cluster++)
    {
        if (!(*refcount_table)[cluster]) {
            contiguous_free_clusters++;
            if (first_gap) {
                /* If this is the first free cluster found, update
                 * *first_free_cluster accordingly */
                *first_free_cluster = cluster;
                first_gap = false;
            }
        } else if (contiguous_free_clusters) {
            contiguous_free_clusters = 0;
        }
    }

    /* If contiguous_free_clusters is greater than zero, it contains the number
     * of continuously free clusters until the current cluster; the first free
     * cluster in the current "gap" is therefore
     * cluster - contiguous_free_clusters */

    /* If no such range could be found, grow the in-memory refcount table
     * accordingly to append free clusters at the end of the image */
    if (contiguous_free_clusters < cluster_count) {
        int64_t old_imrt_nb_clusters = *imrt_nb_clusters;
        uint16_t *new_refcount_table;

        /* contiguous_free_clusters clusters are already empty at the image end;
         * we need cluster_count clusters; therefore, we have to allocate
         * cluster_count - contiguous_free_clusters new clusters at the end of
         * the image (which is the current value of cluster; note that cluster
         * may exceed old_imrt_nb_clusters if *first_free_cluster pointed beyond
         * the image end) */
        *imrt_nb_clusters = cluster + cluster_count - contiguous_free_clusters;
        new_refcount_table = g_try_realloc(*refcount_table,
                                           *imrt_nb_clusters *
                                           sizeof(**refcount_table));
        if (!new_refcount_table) {
            *imrt_nb_clusters = old_imrt_nb_clusters;
            return -ENOMEM;
        }
        *refcount_table = new_refcount_table;

        memset(*refcount_table + old_imrt_nb_clusters, 0,
               (*imrt_nb_clusters - old_imrt_nb_clusters) *
               sizeof(**refcount_table));
    }

    /* Go back to the first free cluster */
    cluster -= contiguous_free_clusters;
    for (i = 0; i < cluster_count; i++) {
        (*refcount_table)[cluster + i] = 1;
    }

    return cluster << s->cluster_bits;
}

/*
 * Creates a new refcount structure based solely on the in-memory information
 * given through *refcount_table. All necessary allocations will be reflected
 * in that array.
 *
 * On success, the old refcount structure is leaked (it will be covered by the
 * new refcount structure).
 */
static int rebuild_refcount_structure(BlockDriverState *bs,
                                      BdrvCheckResult *res,
                                      uint16_t **refcount_table,
                                      int64_t *nb_clusters)
{
    BDRVQcowState *s = bs->opaque;
    int64_t first_free_cluster = 0, reftable_offset = -1, cluster = 0;
    int64_t refblock_offset, refblock_start, refblock_index;
    uint32_t reftable_size = 0;
    uint64_t *on_disk_reftable = NULL;
    uint16_t *on_disk_refblock;
    int i, ret = 0;
    struct {
        uint64_t reftable_offset;
        uint32_t reftable_clusters;
    } QEMU_PACKED reftable_offset_and_clusters;

    qcow2_cache_empty(bs, s->refcount_block_cache);

write_refblocks:
    for (; cluster < *nb_clusters; cluster++) {
        if (!(*refcount_table)[cluster]) {
            continue;
        }

        refblock_index = cluster >> s->refcount_block_bits;
        refblock_start = refblock_index << s->refcount_block_bits;

        /* Don't allocate a cluster in a refblock already written to disk */
        if (first_free_cluster < refblock_start) {
            first_free_cluster = refblock_start;
        }
        refblock_offset = alloc_clusters_imrt(bs, 1, refcount_table,
                                              nb_clusters, &first_free_cluster);
        if (refblock_offset < 0) {
            fprintf(stderr, "ERROR allocating refblock: %s\n",
                    strerror(-refblock_offset));
            res->check_errors++;
            ret = refblock_offset;
            goto fail;
        }

        if (reftable_size <= refblock_index) {
            uint32_t old_reftable_size = reftable_size;
            uint64_t *new_on_disk_reftable;

            reftable_size = ROUND_UP((refblock_index + 1) * sizeof(uint64_t),
                                     s->cluster_size) / sizeof(uint64_t);
            new_on_disk_reftable = g_try_realloc(on_disk_reftable,
                                                 reftable_size *
                                                 sizeof(uint64_t));
            if (!new_on_disk_reftable) {
                res->check_errors++;
                ret = -ENOMEM;
                goto fail;
            }
            on_disk_reftable = new_on_disk_reftable;

            memset(on_disk_reftable + old_reftable_size, 0,
                   (reftable_size - old_reftable_size) * sizeof(uint64_t));

            /* The offset we have for the reftable is now no longer valid;
             * this will leak that range, but we can easily fix that by running
             * a leak-fixing check after this rebuild operation */
            reftable_offset = -1;
        }
        on_disk_reftable[refblock_index] = refblock_offset;

        /* If this is apparently the last refblock (for now), try to squeeze the
         * reftable in */
        if (refblock_index == (*nb_clusters - 1) >> s->refcount_block_bits &&
            reftable_offset < 0)
        {
            uint64_t reftable_clusters = size_to_clusters(s, reftable_size *
                                                          sizeof(uint64_t));
            reftable_offset = alloc_clusters_imrt(bs, reftable_clusters,
                                                  refcount_table, nb_clusters,
                                                  &first_free_cluster);
            if (reftable_offset < 0) {
                fprintf(stderr, "ERROR allocating reftable: %s\n",
                        strerror(-reftable_offset));
                res->check_errors++;
                ret = reftable_offset;
                goto fail;
            }
        }

        ret = qcow2_pre_write_overlap_check(bs, 0, refblock_offset,
                                            s->cluster_size);
        if (ret < 0) {
            fprintf(stderr, "ERROR writing refblock: %s\n", strerror(-ret));
            goto fail;
        }

        on_disk_refblock = qemu_blockalign0(bs->file, s->cluster_size);
        for (i = 0; i < s->refcount_block_size &&
                    refblock_start + i < *nb_clusters; i++)
        {
            on_disk_refblock[i] =
                cpu_to_be16((*refcount_table)[refblock_start + i]);
        }

        ret = bdrv_write(bs->file, refblock_offset / BDRV_SECTOR_SIZE,
                         (void *)on_disk_refblock, s->cluster_sectors);
        qemu_vfree(on_disk_refblock);
        if (ret < 0) {
            fprintf(stderr, "ERROR writing refblock: %s\n", strerror(-ret));
            goto fail;
        }

        /* Go to the end of this refblock */
        cluster = refblock_start + s->refcount_block_size - 1;
    }

    if (reftable_offset < 0) {
        uint64_t post_refblock_start, reftable_clusters;

        post_refblock_start = ROUND_UP(*nb_clusters, s->refcount_block_size);
        reftable_clusters = size_to_clusters(s,
                                             reftable_size * sizeof(uint64_t));
        /* Not pretty but simple */
        if (first_free_cluster < post_refblock_start) {
            first_free_cluster = post_refblock_start;
        }
        reftable_offset = alloc_clusters_imrt(bs, reftable_clusters,
                                              refcount_table, nb_clusters,
                                              &first_free_cluster);
        if (reftable_offset < 0) {
            fprintf(stderr, "ERROR allocating reftable: %s\n",
                    strerror(-reftable_offset));
            res->check_errors++;
            ret = reftable_offset;
            goto fail;
        }

        goto write_refblocks;
    }

    assert(on_disk_reftable);

    for (refblock_index = 0; refblock_index < reftable_size; refblock_index++) {
        cpu_to_be64s(&on_disk_reftable[refblock_index]);
    }

    ret = qcow2_pre_write_overlap_check(bs, 0, reftable_offset,
                                        reftable_size * sizeof(uint64_t));
    if (ret < 0) {
        fprintf(stderr, "ERROR writing reftable: %s\n", strerror(-ret));
        goto fail;
    }

    assert(reftable_size < INT_MAX / sizeof(uint64_t));
    ret = bdrv_pwrite(bs->file, reftable_offset, on_disk_reftable,
                      reftable_size * sizeof(uint64_t));
    if (ret < 0) {
        fprintf(stderr, "ERROR writing reftable: %s\n", strerror(-ret));
        goto fail;
    }

    /* Enter new reftable into the image header */
    cpu_to_be64w(&reftable_offset_and_clusters.reftable_offset,
                 reftable_offset);
    cpu_to_be32w(&reftable_offset_and_clusters.reftable_clusters,
                 size_to_clusters(s, reftable_size * sizeof(uint64_t)));
    ret = bdrv_pwrite_sync(bs->file, offsetof(QCowHeader,
                                              refcount_table_offset),
                           &reftable_offset_and_clusters,
                           sizeof(reftable_offset_and_clusters));
    if (ret < 0) {
        fprintf(stderr, "ERROR setting reftable: %s\n", strerror(-ret));
        goto fail;
    }

    for (refblock_index = 0; refblock_index < reftable_size; refblock_index++) {
        be64_to_cpus(&on_disk_reftable[refblock_index]);
    }
    s->refcount_table = on_disk_reftable;
    s->refcount_table_offset = reftable_offset;
    s->refcount_table_size = reftable_size;

    return 0;

fail:
    g_free(on_disk_reftable);
    return ret;
}

/*
 * Checks an image for refcount consistency.
 *
 * Returns 0 if no errors are found, the number of errors in case the image is
 * detected as corrupted, and -errno when an internal error occurred.
 */
int qcow2_check_refcounts(BlockDriverState *bs, BdrvCheckResult *res,
                          BdrvCheckMode fix)
{
    BDRVQcowState *s = bs->opaque;
    BdrvCheckResult pre_compare_res;
    int64_t size, highest_cluster, nb_clusters;
    uint16_t *refcount_table = NULL;
    bool rebuild = false;
    int ret;

    size = bdrv_getlength(bs->file);
    if (size < 0) {
        res->check_errors++;
        return size;
    }

    nb_clusters = size_to_clusters(s, size);
    if (nb_clusters > INT_MAX) {
        res->check_errors++;
        return -EFBIG;
    }

    res->bfi.total_clusters =
        size_to_clusters(s, bs->total_sectors * BDRV_SECTOR_SIZE);

    ret = calculate_refcounts(bs, res, fix, &rebuild, &refcount_table,
                              &nb_clusters);
    if (ret < 0) {
        goto fail;
    }

    /* In case we don't need to rebuild the refcount structure (but want to fix
     * something), this function is immediately called again, in which case the
     * result should be ignored */
    pre_compare_res = *res;
    compare_refcounts(bs, res, 0, &rebuild, &highest_cluster, refcount_table,
                      nb_clusters);

    if (rebuild && (fix & BDRV_FIX_ERRORS)) {
        BdrvCheckResult old_res = *res;
        int fresh_leaks = 0;

        fprintf(stderr, "Rebuilding refcount structure\n");
        ret = rebuild_refcount_structure(bs, res, &refcount_table,
                                         &nb_clusters);
        if (ret < 0) {
            goto fail;
        }

        res->corruptions = 0;
        res->leaks = 0;

        /* Because the old reftable has been exchanged for a new one the
         * references have to be recalculated */
        rebuild = false;
        memset(refcount_table, 0, nb_clusters * sizeof(uint16_t));
        ret = calculate_refcounts(bs, res, 0, &rebuild, &refcount_table,
                                  &nb_clusters);
        if (ret < 0) {
            goto fail;
        }

        if (fix & BDRV_FIX_LEAKS) {
            /* The old refcount structures are now leaked, fix it; the result
             * can be ignored, aside from leaks which were introduced by
             * rebuild_refcount_structure() that could not be fixed */
            BdrvCheckResult saved_res = *res;
            *res = (BdrvCheckResult){ 0 };

            compare_refcounts(bs, res, BDRV_FIX_LEAKS, &rebuild,
                              &highest_cluster, refcount_table, nb_clusters);
            if (rebuild) {
                fprintf(stderr, "ERROR rebuilt refcount structure is still "
                        "broken\n");
            }

            /* Any leaks accounted for here were introduced by
             * rebuild_refcount_structure() because that function has created a
             * new refcount structure from scratch */
            fresh_leaks = res->leaks;
            *res = saved_res;
        }

        if (res->corruptions < old_res.corruptions) {
            res->corruptions_fixed += old_res.corruptions - res->corruptions;
        }
        if (res->leaks < old_res.leaks) {
            res->leaks_fixed += old_res.leaks - res->leaks;
        }
        res->leaks += fresh_leaks;
    } else if (fix) {
        if (rebuild) {
            fprintf(stderr, "ERROR need to rebuild refcount structures\n");
            res->check_errors++;
            ret = -EIO;
            goto fail;
        }

        if (res->leaks || res->corruptions) {
            *res = pre_compare_res;
            compare_refcounts(bs, res, fix, &rebuild, &highest_cluster,
                              refcount_table, nb_clusters);
        }
    }

    /* check OFLAG_COPIED */
    ret = check_oflag_copied(bs, res, fix);
    if (ret < 0) {
        goto fail;
    }

    res->image_end_offset = (highest_cluster + 1) * s->cluster_size;
    ret = 0;

fail:
    g_free(refcount_table);

    return ret;
}

#define overlaps_with(ofs, sz) \
    ranges_overlap(offset, size, ofs, sz)

/*
 * Checks if the given offset into the image file is actually free to use by
 * looking for overlaps with important metadata sections (L1/L2 tables etc.),
 * i.e. a sanity check without relying on the refcount tables.
 *
 * The ign parameter specifies what checks not to perform (being a bitmask of
 * QCow2MetadataOverlap values), i.e., what sections to ignore.
 *
 * Returns:
 * - 0 if writing to this offset will not affect the mentioned metadata
 * - a positive QCow2MetadataOverlap value indicating one overlapping section
 * - a negative value (-errno) indicating an error while performing a check,
 *   e.g. when bdrv_read failed on QCOW2_OL_INACTIVE_L2
 */
int qcow2_check_metadata_overlap(BlockDriverState *bs, int ign, int64_t offset,
                                 int64_t size)
{
    BDRVQcowState *s = bs->opaque;
    int chk = s->overlap_check & ~ign;
    int i, j;

    if (!size) {
        return 0;
    }

    if (chk & QCOW2_OL_MAIN_HEADER) {
        if (offset < s->cluster_size) {
            return QCOW2_OL_MAIN_HEADER;
        }
    }

    /* align range to test to cluster boundaries */
    size = align_offset(offset_into_cluster(s, offset) + size, s->cluster_size);
    offset = start_of_cluster(s, offset);

    if ((chk & QCOW2_OL_ACTIVE_L1) && s->l1_size) {
        if (overlaps_with(s->l1_table_offset, s->l1_size * sizeof(uint64_t))) {
            return QCOW2_OL_ACTIVE_L1;
        }
    }

    if ((chk & QCOW2_OL_REFCOUNT_TABLE) && s->refcount_table_size) {
        if (overlaps_with(s->refcount_table_offset,
            s->refcount_table_size * sizeof(uint64_t))) {
            return QCOW2_OL_REFCOUNT_TABLE;
        }
    }

    if ((chk & QCOW2_OL_SNAPSHOT_TABLE) && s->snapshots_size) {
        if (overlaps_with(s->snapshots_offset, s->snapshots_size)) {
            return QCOW2_OL_SNAPSHOT_TABLE;
        }
    }

    if ((chk & QCOW2_OL_INACTIVE_L1) && s->snapshots) {
        for (i = 0; i < s->nb_snapshots; i++) {
            if (s->snapshots[i].l1_size &&
                overlaps_with(s->snapshots[i].l1_table_offset,
                s->snapshots[i].l1_size * sizeof(uint64_t))) {
                return QCOW2_OL_INACTIVE_L1;
            }
        }
    }

    if ((chk & QCOW2_OL_ACTIVE_L2) && s->l1_table) {
        for (i = 0; i < s->l1_size; i++) {
            if ((s->l1_table[i] & L1E_OFFSET_MASK) &&
                overlaps_with(s->l1_table[i] & L1E_OFFSET_MASK,
                s->cluster_size)) {
                return QCOW2_OL_ACTIVE_L2;
            }
        }
    }

    if ((chk & QCOW2_OL_REFCOUNT_BLOCK) && s->refcount_table) {
        for (i = 0; i < s->refcount_table_size; i++) {
            if ((s->refcount_table[i] & REFT_OFFSET_MASK) &&
                overlaps_with(s->refcount_table[i] & REFT_OFFSET_MASK,
                s->cluster_size)) {
                return QCOW2_OL_REFCOUNT_BLOCK;
            }
        }
    }

    if ((chk & QCOW2_OL_INACTIVE_L2) && s->snapshots) {
        for (i = 0; i < s->nb_snapshots; i++) {
            uint64_t l1_ofs = s->snapshots[i].l1_table_offset;
            uint32_t l1_sz  = s->snapshots[i].l1_size;
            uint64_t l1_sz2 = l1_sz * sizeof(uint64_t);
            uint64_t *l1 = g_malloc(l1_sz2);
            int ret;

            ret = bdrv_pread(bs->file, l1_ofs, l1, l1_sz2);
            if (ret < 0) {
                g_free(l1);
                return ret;
            }

            for (j = 0; j < l1_sz; j++) {
                uint64_t l2_ofs = be64_to_cpu(l1[j]) & L1E_OFFSET_MASK;
                if (l2_ofs && overlaps_with(l2_ofs, s->cluster_size)) {
                    g_free(l1);
                    return QCOW2_OL_INACTIVE_L2;
                }
            }

            g_free(l1);
        }
    }

    return 0;
}

static const char *metadata_ol_names[] = {
    [QCOW2_OL_MAIN_HEADER_BITNR]    = "qcow2_header",
    [QCOW2_OL_ACTIVE_L1_BITNR]      = "active L1 table",
    [QCOW2_OL_ACTIVE_L2_BITNR]      = "active L2 table",
    [QCOW2_OL_REFCOUNT_TABLE_BITNR] = "refcount table",
    [QCOW2_OL_REFCOUNT_BLOCK_BITNR] = "refcount block",
    [QCOW2_OL_SNAPSHOT_TABLE_BITNR] = "snapshot table",
    [QCOW2_OL_INACTIVE_L1_BITNR]    = "inactive L1 table",
    [QCOW2_OL_INACTIVE_L2_BITNR]    = "inactive L2 table",
};

/*
 * First performs a check for metadata overlaps (through
 * qcow2_check_metadata_overlap); if that fails with a negative value (error
 * while performing a check), that value is returned. If an impending overlap
 * is detected, the BDS will be made unusable, the qcow2 file marked corrupt
 * and -EIO returned.
 *
 * Returns 0 if there were neither overlaps nor errors while checking for
 * overlaps; or a negative value (-errno) on error.
 */
int qcow2_pre_write_overlap_check(BlockDriverState *bs, int ign, int64_t offset,
                                  int64_t size)
{
    int ret = qcow2_check_metadata_overlap(bs, ign, offset, size);

    if (ret < 0) {
        return ret;
    } else if (ret > 0) {
        int metadata_ol_bitnr = ffs(ret) - 1;
        assert(metadata_ol_bitnr < QCOW2_OL_MAX_BITNR);

        qcow2_signal_corruption(bs, true, offset, size, "Preventing invalid "
                                "write on metadata (overlaps with %s)",
                                metadata_ol_names[metadata_ol_bitnr]);
        return -EIO;
    }

    return 0;
}
