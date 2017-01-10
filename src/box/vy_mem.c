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

#define BPS_TREE_SOURCE 1
#include "vy_mem.h"

#include <trivia/util.h>
#include <small/lsregion.h>
#include "diag.h"

#include "vy_stmt.h"

int
vy_mem_tree_cmp(const struct vy_stmt *a, const struct vy_stmt *b,
		struct vy_mem *index)
{
	int res = vy_stmt_compare(a, b, index->format, index->key_def);
	res = res ? res : a->lsn > b->lsn ? -1 : a->lsn < b->lsn;
	return res;
}

int
vy_mem_tree_cmp_key(const struct vy_stmt *a, struct tree_mem_key *key,
		    struct vy_mem *index)
{
	int res = vy_stmt_compare(a, key->stmt, index->format, index->key_def);
	if (res == 0) {
		if (key->lsn == INT64_MAX - 1)
			return 0;
		res = a->lsn > key->lsn ? -1 : a->lsn < key->lsn;
	}
	return res;
}

static void *
vy_mem_tree_extent_alloc(void *ctx)
{
	struct vy_mem *mem = (struct vy_mem *) ctx;
	assert(mem->allocator != NULL && mem->allocator_lsn != NULL);
	void *ret = lsregion_alloc(mem->allocator, VY_MEM_TREE_EXTENT_SIZE,
				   *mem->allocator_lsn);
	if (ret == NULL)
		diag_set(OutOfMemory, VY_MEM_TREE_EXTENT_SIZE, "lsregion_alloc",
			 "ret");
	return ret;
}

static void
vy_mem_tree_extent_free(void *ctx, void *p)
{
	/* Can't free part of region allocated memory. */
	(void)ctx;
	(void)p;
}

struct vy_mem *
vy_mem_new(struct key_def *key_def, struct tuple_format *format,
	   struct lsregion *allocator, const int64_t *allocator_lsn)
{
	struct vy_mem *index = malloc(sizeof(*index));
	if (!index) {
		diag_set(OutOfMemory, sizeof(*index),
			 "malloc", "struct vy_mem");
		return NULL;
	}
	index->min_lsn = INT64_MAX;
	index->used = 0;
	index->key_def = key_def;
	index->version = 0;
	index->format = format;
	index->allocator = allocator;
	index->allocator_lsn = allocator_lsn;
	vy_mem_tree_create(&index->tree, index, vy_mem_tree_extent_alloc,
			   vy_mem_tree_extent_free, index);
	rlist_create(&index->in_frozen);
	rlist_create(&index->in_dirty);
	return index;
}

void
vy_mem_delete(struct vy_mem *index)
{
	assert(index == index->tree.arg);
	TRASH(index);
	free(index);
}

/*
 * Return the older statement for the given one.
 */
const struct vy_stmt *
vy_mem_older_lsn(struct vy_mem *mem, const struct vy_stmt *stmt,
		 const struct key_def *key_def)
{
	struct tree_mem_key tree_key;
	tree_key.stmt = stmt;
	tree_key.lsn = stmt->lsn - 1;
	bool exact = false;
	struct vy_mem_tree_iterator itr =
		vy_mem_tree_lower_bound(&mem->tree, &tree_key, &exact);

	if (vy_mem_tree_iterator_is_invalid(&itr))
		return NULL;

	const struct vy_stmt *result;
	result = *vy_mem_tree_iterator_get_elem(&mem->tree, &itr);
	if (vy_stmt_compare(result, stmt, mem->format, key_def) != 0)
		return NULL;
	return result;
}
