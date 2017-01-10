#ifndef INCLUDES_TARANTOOL_BOX_VY_MEM_H
#define INCLUDES_TARANTOOL_BOX_VY_MEM_H
/*
 * Copyright 2010-2017, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * AUTHORS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>
#include <stdbool.h>

#include <small/rlist.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_mem;
struct vy_stmt;
struct lsregion;

struct tree_mem_key {
	const struct vy_stmt *stmt;
	int64_t lsn;
};

int
vy_mem_tree_cmp(const struct vy_stmt *a, const struct vy_stmt *b,
		struct vy_mem *index);
int
vy_mem_tree_cmp_key(const struct vy_stmt *a, struct tree_mem_key *key,
		    struct vy_mem *index);

#define VY_MEM_TREE_EXTENT_SIZE (16 * 1024)

#define BPS_TREE_NAME vy_mem_tree
#define BPS_TREE_BLOCK_SIZE 512
#define BPS_TREE_EXTENT_SIZE VY_MEM_TREE_EXTENT_SIZE
#define BPS_TREE_COMPARE(a, b, index) vy_mem_tree_cmp(a, b, index)
#define BPS_TREE_COMPARE_KEY(a, b, index) vy_mem_tree_cmp_key(a, b, index)
#define bps_tree_elem_t const struct vy_stmt *
#define bps_tree_key_t struct tree_mem_key *
#define bps_tree_arg_t struct vy_mem *
#define BPS_TREE_NO_DEBUG

#include "salad/bps_tree.h"

#undef BPS_TREE_NAME
#undef BPS_TREE_BLOCK_SIZE
#undef BPS_TREE_EXTENT_SIZE
#undef BPS_TREE_COMPARE
#undef BPS_TREE_COMPARE_KEY
#undef bps_tree_elem_t
#undef bps_tree_key_t
#undef bps_tree_arg_t
#undef BPS_TREE_NO_DEBUG

/*
 * vy_mem is an in-memory container for vy_stmt objects in
 * a single vinyl range.
 * Internally it uses bps_tree to stores struct vy_stmt *objects.
 * which are ordered by statement key and, for the same key,
 * by lsn, in descending order.
 *
 * For example, assume there are two statements with the same key,
 * but different LSN. These are duplicates of the same key,
 * maintained for the purpose of MVCC/consistent read view.
 * In Vinyl terms, they form a duplicate chain.
 *
 * vy_mem distinguishes between the first duplicate in the chain
 * and other keys in that chain.
 *
 * During insertion, the reference counter of vy_stmt is
 * incremented, during destruction all vy_stmt' reference
 * counters are decremented.
 */
struct vy_mem {
	/** Link in range->frozen list. */
	struct rlist in_frozen;
	/** Link in scheduler->dirty_mems list. */
	struct rlist in_dirty;
	/** BPS tree */
	struct vy_mem_tree tree;
	/** The total size of all tuples in this tree in bytes */
	size_t used;
	/** The minimum value of stmt->lsn in this tree */
	int64_t min_lsn;
	/* A key definition for this index. */
	struct key_def *key_def;
	/* A tuple format for key_def. */
	struct tuple_format *format;
	/** Version is initially 0 and is incremented on every write */
	uint32_t version;
	/** Allocator for extents */
	struct lsregion *allocator;
	/** The last LSN for lsregion allocator */
	const int64_t *allocator_lsn;
};

/**
 * Instantiate a new in-memory tree.
 *
 * @param key_def key definition.
 * @param format tuple format.
 * @param allocator lsregioni allocator to use for BPS tree extents
 * @param allocator_lsn a pointer to the latest LSN for lsregion.
 * @retval new vy_mem instance on success.
 * @retval NULL on error, check diag.
 */
struct vy_mem *
vy_mem_new(struct key_def *key_def, struct tuple_format *format,
	   struct lsregion *allocator, const int64_t *allocator_lsn);

/**
 * Delete in-memory tree.
 */
void
vy_mem_delete(struct vy_mem *index);

/**
 * Insert a statement into the tree.
 *
 * @param mem vy_mem
 * @param stmt statement
 * @param alloc_lsn LSN for lsregion allocator
 * @retval 0 on success
 * @retval -1 on error, check diag
 */
int
vy_mem_insert(struct vy_mem *mem, const struct vy_stmt *stmt,
	      int64_t alloc_lsn);

/**
 * Return the older statement for the given key.
 *
 * @param mem vy_mem
 * @param key key
 * @param key_def key definition
 * @retval stmt the older statement for the @a key.
 * @retval NULL if there is no other statements with lsn less than
 * key->lsn.
 */
const struct vy_stmt *
vy_mem_older_lsn(struct vy_mem *mem, const struct vy_stmt *key,
		 const struct key_def *key_def);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_MEM_H */
