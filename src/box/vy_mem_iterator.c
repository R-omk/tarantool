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

#include "vy_mem_iterator.h"

#include "vy_stmt.h"

/* {{{ vy_mem_iterator support functions */

static int
vy_mem_iterator_copy_to(struct vy_mem_iterator *itr, struct vy_stmt **ret)
{
	assert(itr->curr_stmt != NULL);
	if (itr->last_stmt)
		vy_stmt_unref(itr->last_stmt);
	itr->last_stmt = vy_stmt_dup(itr->curr_stmt);
	*ret = itr->last_stmt;
	if (itr->last_stmt != NULL)
		return 0;
	return -1;
}

/**
 * Get a stmt by current position
 */
static const struct vy_stmt *
vy_mem_iterator_curr_stmt(struct vy_mem_iterator *itr)
{
	return *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &itr->curr_pos);
}

/**
 * Make a step in directions defined by itr->iterator_type
 * @retval 0 success
 * @retval 1 EOF
 */
static int
vy_mem_iterator_step(struct vy_mem_iterator *itr)
{
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	else
		vy_mem_tree_iterator_next(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr_stmt = vy_mem_iterator_curr_stmt(itr);
	return 0;
}

/**
 * Find next record with lsn <= itr->lsn record.
 * Current position must be at the beginning of serie of records with the
 * same key it terms of direction of iterator (i.e. left for GE, right for LE)
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_find_lsn(struct vy_mem_iterator *itr)
{
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;
	while (itr->curr_stmt->lsn > *itr->vlsn) {
		if (vy_mem_iterator_step(itr) != 0 ||
		    (itr->iterator_type == ITER_EQ &&
		     vy_stmt_compare(itr->key, itr->curr_stmt, format,
				     key_def))) {
			itr->curr_stmt = NULL;
			return 1;
		}
	}
	if (itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT) {
		struct vy_mem_tree_iterator prev_pos = itr->curr_pos;
		vy_mem_tree_iterator_prev(&itr->mem->tree, &prev_pos);

		while (!vy_mem_tree_iterator_is_invalid(&prev_pos)) {
			const struct vy_stmt *prev_stmt =
				*vy_mem_tree_iterator_get_elem(&itr->mem->tree,
							       &prev_pos);
			if (prev_stmt->lsn > *itr->vlsn ||
			    vy_stmt_compare(itr->curr_stmt, prev_stmt, format,
					    key_def) != 0)
				break;
			itr->curr_pos = prev_pos;
			itr->curr_stmt = prev_stmt;
			vy_mem_tree_iterator_prev(&itr->mem->tree, &prev_pos);
		}
	}
	assert(itr->curr_stmt != NULL);
	return 0;
}

/**
 * Find next (lower, older) record with the same key as current
 *
 * @retval 0 Found
 * @retval 1 Not found
 */
static int
vy_mem_iterator_start(struct vy_mem_iterator *itr)
{
	assert(!itr->search_started);
	itr->search_started = true;
	itr->version = itr->mem->version;

	struct tree_mem_key tree_key;
	tree_key.stmt = itr->key;
	/* (lsn == INT64_MAX - 1) means that lsn is ignored in comparison */
	tree_key.lsn = INT64_MAX - 1;
	if (vy_stmt_part_count(itr->key) > 0) {
		if (itr->iterator_type == ITER_EQ) {
			bool exact;
			itr->curr_pos =
				vy_mem_tree_lower_bound(&itr->mem->tree,
							&tree_key, &exact);
			if (!exact)
				return 1;
		} else if (itr->iterator_type == ITER_LE ||
			   itr->iterator_type == ITER_GT) {
			itr->curr_pos =
				vy_mem_tree_upper_bound(&itr->mem->tree,
							&tree_key, NULL);
		} else {
			assert(itr->iterator_type == ITER_GE ||
			       itr->iterator_type == ITER_LT);
			itr->curr_pos =
				vy_mem_tree_lower_bound(&itr->mem->tree,
							&tree_key, NULL);
		}
	} else if (itr->iterator_type == ITER_LE) {
		itr->curr_pos = vy_mem_tree_invalid_iterator();
	} else {
		assert(itr->iterator_type == ITER_GE);
		itr->curr_pos = vy_mem_tree_iterator_first(&itr->mem->tree);
	}

	if (itr->iterator_type == ITER_LT || itr->iterator_type == ITER_LE)
		vy_mem_tree_iterator_prev(&itr->mem->tree, &itr->curr_pos);
	if (vy_mem_tree_iterator_is_invalid(&itr->curr_pos))
		return 1;
	itr->curr_stmt = vy_mem_iterator_curr_stmt(itr);
	return vy_mem_iterator_find_lsn(itr);
}

/**
 * Restores iterator if the mem have been changed
 */
static void
vy_mem_iterator_check_version(struct vy_mem_iterator *itr)
{
	assert(itr->curr_stmt != NULL);
	if (itr->version == itr->mem->version)
		return;
	itr->version = itr->mem->version;
	const struct vy_stmt * const *record;
	record = vy_mem_tree_iterator_get_elem(&itr->mem->tree, &itr->curr_pos);
	if (record != NULL && *record == itr->curr_stmt)
		return;
	struct tree_mem_key tree_key;
	tree_key.stmt = itr->curr_stmt;
	tree_key.lsn = itr->curr_stmt->lsn;
	bool exact;
	itr->curr_pos = vy_mem_tree_lower_bound(&itr->mem->tree,
						&tree_key, &exact);
	assert(exact);
}

/* }}} vy_mem_iterator support functions */

/* {{{ vy_mem_iterator API implementation */
/* TODO: move to c file and remove static keyword */

static const struct vy_stmt_iterator_iface vy_mem_iterator_iface;

/**
 * Open the iterator.
 */
void
vy_mem_iterator_open(struct vy_mem_iterator *itr, struct vy_mem *mem,
		     enum iterator_type iterator_type,
		     const struct vy_stmt *key, const int64_t *vlsn)
{
	itr->base.iface = &vy_mem_iterator_iface;

	assert(key != NULL);
	itr->mem = mem;

	itr->iterator_type = iterator_type;
	itr->key = key;
	itr->vlsn = vlsn;
	if (vy_stmt_part_count(key) == 0) {
		/* NULL key. change itr->iterator_type for simplification */
		itr->iterator_type = iterator_type == ITER_LT ||
				     iterator_type == ITER_LE ?
				     ITER_LE : ITER_GE;
	}

	itr->curr_pos = vy_mem_tree_invalid_iterator();
	itr->curr_stmt = NULL;
	itr->last_stmt = NULL;

	itr->search_started = false;
}

/*
 * Find the next record with different key as current and visible lsn.
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_key_impl(struct vy_mem_iterator *itr)
{
	if (!itr->search_started)
		return vy_mem_iterator_start(itr);
	if (!itr->curr_stmt) /* End of search. */
		return 1;
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	vy_mem_iterator_check_version(itr);
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;

	const struct vy_stmt *prev_stmt = itr->curr_stmt;
	do {
		if (vy_mem_iterator_step(itr) != 0) {
			itr->curr_stmt = NULL;
			return 1;
		}
	} while (vy_stmt_compare(prev_stmt, itr->curr_stmt, format,
				 key_def) == 0);

	if (itr->iterator_type == ITER_EQ &&
	    vy_stmt_compare(itr->key, itr->curr_stmt, format, key_def) != 0) {
		itr->curr_stmt = NULL;
		return 1;
	}
	return vy_mem_iterator_find_lsn(itr);
}

/**
 * Find the next record with different key as current and visible lsn.
 * @retval 0 success or EOF (*ret == NULL)
 */
static NODISCARD int
vy_mem_iterator_next_key(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_key == vy_mem_iterator_next_key);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	*ret = NULL;

	if (vy_mem_iterator_next_key_impl(itr) == 0)
		return vy_mem_iterator_copy_to(itr, ret);
	return 0;
}

/*
 * Find next (lower, older) record with the same key as current
 * @retval 0 Found
 * @retval 1 Not found
 */
static NODISCARD int
vy_mem_iterator_next_lsn_impl(struct vy_mem_iterator *itr)
{
	if (!itr->search_started)
		return vy_mem_iterator_start(itr);
	if (!itr->curr_stmt) /* End of search. */
		return 1;
	assert(!vy_mem_tree_iterator_is_invalid(&itr->curr_pos));
	vy_mem_iterator_check_version(itr);
	assert(itr->curr_stmt == vy_mem_iterator_curr_stmt(itr));
	struct key_def *key_def = itr->mem->key_def;

	struct vy_mem_tree_iterator next_pos = itr->curr_pos;
	vy_mem_tree_iterator_next(&itr->mem->tree, &next_pos);
	if (vy_mem_tree_iterator_is_invalid(&next_pos))
		return 1; /* EOF */

	const struct vy_stmt *next_stmt;
	next_stmt = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &next_pos);
	if (vy_stmt_compare(itr->curr_stmt, next_stmt, itr->mem->format,
			    key_def) == 0) {
		itr->curr_pos = next_pos;
		itr->curr_stmt = next_stmt;
		return 0;
	}
	return 1;
}

/**
 * Find next (lower, older) record with the same key as current
 * @retval 0 success or EOF (*ret == NULL)
 */
static NODISCARD int
vy_mem_iterator_next_lsn(struct vy_stmt_iterator *vitr, struct vy_stmt *in,
			 struct vy_stmt **ret)
{
	(void)in;
	assert(vitr->iface->next_lsn == vy_mem_iterator_next_lsn);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	*ret = NULL;
	if (vy_mem_iterator_next_lsn_impl(itr) == 0)
		return vy_mem_iterator_copy_to(itr, ret);
	return 0;
}

/**
 * Restore the current position (if necessary).
 * @sa struct vy_stmt_iterator comments.
 *
 * @param last_stmt the key the iterator was positioned on
 *
 * @retval 0 nothing changed
 * @retval 1 iterator position was changed
 */
static NODISCARD int
vy_mem_iterator_restore(struct vy_stmt_iterator *vitr,
			const struct vy_stmt *last_stmt, struct vy_stmt **ret)
{
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	struct key_def *def = itr->mem->key_def;
	struct tuple_format *format = itr->mem->format;
	int rc;
	*ret = NULL;

	if (!itr->search_started) {
		if (last_stmt == NULL) {
			if (vy_mem_iterator_start(itr) == 0)
				return vy_mem_iterator_copy_to(itr, ret);
			return 0;
		}

		/*
		 * Restoration is very similar to first search so we'll use
		 * that.
		 */
		enum iterator_type save_type = itr->iterator_type;
		const struct vy_stmt *save_key = itr->key;
		if (itr->iterator_type == ITER_GT ||
		    itr->iterator_type == ITER_EQ)
			itr->iterator_type = ITER_GE;
		else if (itr->iterator_type == ITER_LT)
			itr->iterator_type = ITER_LE;
		itr->key = last_stmt;
		rc = vy_mem_iterator_start(itr);
		itr->iterator_type = save_type;
		itr->key = save_key;
		if (rc > 0) /* Search ended. */
			return 0;
		bool position_changed = true;
		if (vy_stmt_compare(itr->curr_stmt, last_stmt, format,
				    def) == 0) {
			position_changed = false;
			if (itr->curr_stmt->lsn >= last_stmt->lsn) {
				/*
				 * Skip the same stmt to next stmt or older
				 * version.
				 */
				do {
					rc = vy_mem_iterator_next_lsn_impl(itr);
					if (rc == 0) /* Move further. */
						continue;
					assert(rc > 0);
					rc = vy_mem_iterator_next_key_impl(itr);
					assert(rc >= 0);
					break;
				} while (itr->curr_stmt->lsn >=
					 last_stmt->lsn);
				if (itr->curr_stmt != NULL)
					position_changed = true;
			}
		} else if (itr->iterator_type == ITER_EQ &&
			   vy_stmt_compare(itr->key, itr->curr_stmt, format,
					   def) != 0) {
			return true;
		}
		if (itr->curr_stmt != NULL &&
		    vy_mem_iterator_copy_to(itr, ret) < 0)
			return -1;
		return position_changed;
	}

	if (itr->version == itr->mem->version) {
		if (itr->curr_stmt)
			return vy_mem_iterator_copy_to(itr, ret);
		return 0;
	}

	if (last_stmt == NULL || itr->curr_stmt == NULL) {
		itr->version = itr->mem->version;
		const struct vy_stmt *was_stmt = itr->curr_stmt;
		itr->search_started = false;
		itr->curr_stmt = NULL;
		vy_mem_iterator_start(itr);
		return was_stmt != itr->curr_stmt;
	}

	vy_mem_iterator_check_version(itr);
	struct vy_mem_tree_iterator pos = itr->curr_pos;
	rc = 0;
	if (itr->iterator_type == ITER_GE || itr->iterator_type == ITER_GT ||
	    itr->iterator_type == ITER_EQ) {
		while (true) {
			vy_mem_tree_iterator_prev(&itr->mem->tree, &pos);
			if (vy_mem_tree_iterator_is_invalid(&pos))
				break;
			const struct vy_stmt *t;
			t = *vy_mem_tree_iterator_get_elem(&itr->mem->tree,
							   &pos);
			int cmp;
			cmp = vy_stmt_compare(t, last_stmt, format, def);
			if (cmp < 0 || (cmp == 0 && t->lsn >= last_stmt->lsn))
				break;
			if (t->lsn <= *itr->vlsn) {
				itr->curr_pos = pos;
				itr->curr_stmt = t;
				rc = 1;
			}
		}
		if (vy_mem_iterator_copy_to(itr, ret) < 0)
			return -1;
		return rc;
	}
	assert(itr->iterator_type == ITER_LE || itr->iterator_type == ITER_LT);
	int cmp;
	cmp = vy_stmt_compare(itr->curr_stmt, last_stmt, format, def);
	int64_t break_lsn = cmp == 0 ? last_stmt->lsn : *itr->vlsn + 1;
	while (true) {
		vy_mem_tree_iterator_prev(&itr->mem->tree, &pos);
		if (vy_mem_tree_iterator_is_invalid(&pos))
			break;
		const struct vy_stmt *t;
		t = *vy_mem_tree_iterator_get_elem(&itr->mem->tree, &pos);
		int cmp;
		cmp = vy_stmt_compare(t, itr->curr_stmt, format, def);
		assert(cmp <= 0);
		if (cmp < 0 || t->lsn >= break_lsn)
			break;
		itr->curr_pos = pos;
		itr->curr_stmt = t;
		rc = 1;
	}
	if (vy_mem_iterator_copy_to(itr, ret) < 0)
		return -1;
	return rc;
}

/**
 * Close an iterator and free all resources
 */
static void
vy_mem_iterator_close(struct vy_stmt_iterator *vitr)
{
	assert(vitr->iface->close == vy_mem_iterator_close);
	struct vy_mem_iterator *itr = (struct vy_mem_iterator *) vitr;
	if (itr->last_stmt != NULL)
		vy_stmt_unref(itr->last_stmt);
	TRASH(itr);
}

static const struct vy_stmt_iterator_iface vy_mem_iterator_iface = {
	.next_key = vy_mem_iterator_next_key,
	.next_lsn = vy_mem_iterator_next_lsn,
	.restore = vy_mem_iterator_restore,
	.close = vy_mem_iterator_close
};

/* }}} vy_mem_iterator API implementation */
