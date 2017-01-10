#ifndef INCLUDES_TARANTOOL_BOX_VY_MEM_ITERATOR_H
#define INCLUDES_TARANTOOL_BOX_VY_MEM_ITERATOR_H
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

#include "index.h" /* enum iterator_type */
#include "vy_mem.h" /* struct vy_mem_tree_iterator */
#include "vy_stmt_iterator.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

struct vy_mem;
struct vy_stmt;

/**
 * Return statements from vy_mem (in-memory index) based on
 * initial search key, iteration order and view lsn.
 *
 * All statements with lsn > vlsn are skipped.
 * The API allows to traverse over resulting statements within two
 * dimensions - key and lsn. next_key() switches to the youngest
 * statement of the next key, according to the iteration order,
 * and next_lsn() switches to an older statement for the same
 * key.
 */
struct vy_mem_iterator {
	/** Parent class, must be the first member */
	struct vy_stmt_iterator base;

	/* mem */
	struct vy_mem *mem;

	/* Search options */
	/**
	 * Iterator type, that specifies direction, start position and stop
	 * criteria if key == NULL: GT and EQ are changed to GE, LT to LE for
	 * beauty.
	 */
	enum iterator_type iterator_type;
	/** Key to search. */
	const struct vy_stmt *key;
	/* LSN visibility, iterator shows values with lsn <= than that */
	const int64_t *vlsn;

	/* State of iterator */
	/* Current position in tree */
	struct vy_mem_tree_iterator curr_pos;
	/*
	 * The pointer on a region allocated statement from vy_mem BPS tree.
	 * There is no guarantee that curr_pos points on curr_stmt in the tree.
	 * For example, cur_pos can be invalid but curr_stmt can point on a
	 * valid statement.
	 */
	const struct vy_stmt *curr_stmt;
	/*
	 * Copy of the statement returned from one of public methods
	 * (restore/next_lsn/next_key). Need to store the copy, because can't
	 * return region allocated curr_stmt.
	 */
	struct vy_stmt *last_stmt;
	/* data version from vy_mem */
	uint32_t version;

	/* Is false until first .._next_.. method is called */
	bool search_started;
};

void
vy_mem_iterator_open(struct vy_mem_iterator *itr, struct vy_mem *mem,
		     enum iterator_type iterator_type,
		     const struct vy_stmt *key, const int64_t *vlsn);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* INCLUDES_TARANTOOL_BOX_VY_MEM_ITERATOR_H */
