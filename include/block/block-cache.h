/*
 * QEMU Block Layer Cache
 *
 * Copyright IBM, Corp. 2012
 *
 * Authors:
 *  Dong Xu Wang <wdongxu@linux.vnet.ibm.com>
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

#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

typedef enum {
    BLOCK_TABLE_REF,
    BLOCK_TABLE_L2,
    BLOCK_TABLE_BITMAP,
} BlockTableType;

struct BlockCache;
typedef struct BlockCache BlockCache;

BlockCache *block_cache_create(BlockDriverState *bs, int num_tables,
                               size_t table_size, BlockTableType type);
int block_cache_destroy(BlockDriverState *bs, BlockCache *c);
int block_cache_flush(BlockDriverState *bs, BlockCache *c);
int block_cache_set_dependency(BlockDriverState *bs,
                               BlockCache *c,
                               BlockCache *dependency);
void block_cache_depends_on_flush(BlockCache *c);
int block_cache_get(BlockDriverState *bs, BlockCache *c, uint64_t offset,
                    void **table);
int block_cache_get_empty(BlockDriverState *bs, BlockCache *c,
                          uint64_t offset, void **table);
int block_cache_put(BlockDriverState *bs, BlockCache *c, void **table);
void block_cache_entry_mark_dirty(BlockCache *c, void *table);
#endif /* BLOCK_CACHE_H */
