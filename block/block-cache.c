/*
 * QEMU Block Layer Cache
 *
 * This file is based on qcow2-cache.c, see its copyrights below:
 *
 * L2/refcount table cache for the QCOW2 format
 *
 * Copyright (c) 2010 Kevin Wolf <kwolf@redhat.com>
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

#include "block/block_int.h"
#include "qemu-common.h"
#include "trace.h"
#include "block/block-cache.h"

typedef struct BlockCachedTable {
    void    *table;
    int64_t offset;
    bool    dirty;
    int     cache_hits;
    int     ref;
} BlockCachedTable;

struct BlockCache {
    BlockCachedTable    *entries;
    struct BlockCache   *depends;
    int                 size;
    size_t              table_size;
    BlockTableType      table_type;
    bool                depends_on_flush;
};

BlockCache *block_cache_create(BlockDriverState *bs, int num_tables,
                               size_t table_size, BlockTableType type)
{
    BlockCache *c;
    int i;

    c = g_malloc0(sizeof(*c));
    c->size = num_tables;
    c->entries = g_malloc0(sizeof(*c->entries) * num_tables);
    c->table_type = type;
    c->table_size = table_size;

    for (i = 0; i < c->size; i++) {
        c->entries[i].table = qemu_blockalign(bs, table_size);
    }

    return c;
}

int block_cache_destroy(BlockDriverState *bs, BlockCache *c)
{
    int i;

    for (i = 0; i < c->size; i++) {
        assert(c->entries[i].ref == 0);
        qemu_vfree(c->entries[i].table);
    }

    g_free(c->entries);
    g_free(c);

    return 0;
}

static int block_cache_flush_dependency(BlockDriverState *bs, BlockCache *c)
{
    int ret;

    ret = block_cache_flush(bs, c->depends);
    if (ret < 0) {
        return ret;
    }

    c->depends = NULL;
    c->depends_on_flush = false;

    return 0;
}

static int block_cache_entry_flush(BlockDriverState *bs, BlockCache *c, int i)
{
    int ret = 0;

    if (!c->entries[i].dirty || !c->entries[i].offset) {
        return 0;
    }

    trace_block_cache_entry_flush(qemu_coroutine_self(), c->table_type, i);

    if (c->depends) {
        ret = block_cache_flush_dependency(bs, c);
    } else if (c->depends_on_flush) {
        ret = bdrv_flush(bs->file);
        if (ret >= 0) {
            c->depends_on_flush = false;
        }
    }

    if (ret < 0) {
        return ret;
    }

    if (c->table_type == BLOCK_TABLE_REF) { BLKDBG_EVENT(bs->file, BLKDBG_REFBLOCK_UPDATE_PART);
    } else if (c->table_type == BLOCK_TABLE_L2) {
        BLKDBG_EVENT(bs->file, BLKDBG_L2_UPDATE);
    } else if (c->table_type == BLOCK_TABLE_BITMAP) {
        BLKDBG_EVENT(bs->file, BLKDBG_ADDCOW_WRITE);
    }

    ret = bdrv_pwrite(bs->file, c->entries[i].offset,
                      c->entries[i].table, c->table_size);
    if (ret < 0) {
        return ret;
    }

    c->entries[i].dirty = false;

    return 0;
}

int block_cache_flush(BlockDriverState *bs, BlockCache *c)
{
    int result = 0;
    int ret;
    int i;

    trace_block_cache_flush(qemu_coroutine_self(), c->table_type);

    for (i = 0; i < c->size; i++) {
        ret = block_cache_entry_flush(bs, c, i);
        if (ret < 0 && result != -ENOSPC) {
            result = ret;
        }
    }

    if (result == 0) {
        ret = bdrv_flush(bs->file);
        if (ret < 0) {
            result = ret;
        }
    }

    return result;
}

int block_cache_set_dependency_two_bs(BlockDriverState *bs,
                                      BlockCache *c,
                                      BlockDriverState *depend_bs,
                                      BlockCache *dependency)
{
    int ret;

    if (dependency->depends) {
        ret = block_cache_flush_dependency(depend_bs, dependency);
        if (ret < 0) {
            return ret;
        }
    }

    if (c->depends && (c->depends != dependency)) {
        ret = block_cache_flush_dependency(bs, c);
        if (ret < 0) {
            return ret;
        }
    }

    c->depends = dependency;
    return 0;
}

int block_cache_set_dependency(BlockDriverState *bs,
                               BlockCache *c,
                               BlockCache *dependency)
{
    return block_cache_set_dependency_two_bs(bs, c, bs, dependency);
}

void block_cache_depends_on_flush(BlockCache *c)
{
    c->depends_on_flush = true;
}

static int block_cache_find_entry_to_replace(BlockCache *c)
{
    int i;
    int min_count = INT_MAX;
    int min_index = -1;


    for (i = 0; i < c->size; i++) {
        if (c->entries[i].ref) {
            continue;
        }

        if (c->entries[i].cache_hits < min_count) {
            min_index = i;
            min_count = c->entries[i].cache_hits;
        }

        /* Give newer hits priority */
        /* TODO Check how to optimize the replacement strategy */
        c->entries[i].cache_hits /= 2;
    }

    if (min_index == -1) {
        /* This can't happen in current synchronous code, but leave the check
         * here as a reminder for whoever starts using AIO with the cache */
        abort();
    }
    return min_index;
}

static int block_cache_do_get(BlockDriverState *bs, BlockCache *c,
                              uint64_t offset, void **table,
                              bool read_from_disk)
{
    int i;
    int ret;

    trace_block_cache_get(qemu_coroutine_self(), c->table_type,
                          offset, read_from_disk);

    /* Check if the table is already cached */
    for (i = 0; i < c->size; i++) {
        if (c->entries[i].offset == offset) {
            goto found;
        }
    }

    /* If not, write a table back and replace it */
    i = block_cache_find_entry_to_replace(c);
    trace_block_cache_get_replace_entry(qemu_coroutine_self(),
                                        c->table_type, i);
    if (i < 0) {
        return i;
    }

    ret = block_cache_entry_flush(bs, c, i);
    if (ret < 0) {
        return ret;
    }

    trace_block_cache_get_read(qemu_coroutine_self(),
                               c->table_type, i);
    c->entries[i].offset = 0;
    if (read_from_disk) {
        if (c->table_type == BLOCK_TABLE_L2) {
            BLKDBG_EVENT(bs->file, BLKDBG_L2_LOAD);
        } else if (c->table_type == BLOCK_TABLE_BITMAP) {
            BLKDBG_EVENT(bs->file, BLKDBG_ADDCOW_READ);
        }

        ret = bdrv_pread(bs->file, offset, c->entries[i].table,
                         c->table_size);
        if (ret < 0) {
            return ret;
        }
    }

    /* Give the table some hits for the start so that it won't be replaced
     * immediately. The number 32 is completely arbitrary. */
    c->entries[i].cache_hits = 32;
    c->entries[i].offset = offset;

    /* And return the right table */
found:
    c->entries[i].cache_hits++;
    c->entries[i].ref++;
    *table = c->entries[i].table;

    trace_block_cache_get_done(qemu_coroutine_self(),
                               c->table_type, i);

    return 0;
}

int block_cache_get(BlockDriverState *bs, BlockCache *c, uint64_t offset,
                    void **table)
{
    return block_cache_do_get(bs, c, offset, table, true);
}

int block_cache_get_empty(BlockDriverState *bs, BlockCache *c,
                          uint64_t offset, void **table)
{
    return block_cache_do_get(bs, c, offset, table, false);
}

int block_cache_put(BlockDriverState *bs, BlockCache *c, void **table)
{
    int i;

    for (i = 0; i < c->size; i++) {
        if (c->entries[i].table == *table) {
            goto found;
        }
    }
    return -ENOENT;

found:
    c->entries[i].ref--;
    *table = NULL;

    assert(c->entries[i].ref >= 0);
    return 0;
}

void block_cache_entry_mark_dirty(BlockCache *c, void *table)
{
    int i;

    for (i = 0; i < c->size; i++) {
        if (c->entries[i].table == table) {
            goto found;
        }
    }
    abort();

found:
    c->entries[i].dirty = true;
}
