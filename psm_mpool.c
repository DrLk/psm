/*
 * Copyright (c) 2006-2010. QLogic Corporation. All rights reserved.
 * Copyright (c) 2003-2006, PathScale, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "psm_user.h"

#define PSMI_MPOOL_ALIGNMENT	64

struct mpool_element {
    union {
	SLIST_ENTRY(mpool_element) me_next;
	mpool_t me_mpool;
    };

    uint32_t me_gen_count;
    uint32_t me_index;
#ifdef PSM_DEBUG
    uint32_t me_isused;
#endif
} __attribute__ ((aligned(8)));

#ifdef PSM_DEBUG
#  define me_mark_used(me)    ((me)->me_isused = 1)
#  define me_mark_unused(me)  ((me)->me_isused = 0)
#else
#  define me_mark_used(me)
#  define me_mark_unused(me)
#endif

struct mpool {
    int mp_type;
    int mp_flags;
    int mp_vector_shift;
    
    uint32_t mp_elm_vector_size;
    uint32_t mp_elm_offset;
    uint32_t mp_num_obj;
    uint32_t mp_num_obj_inuse;
    uint32_t mp_elm_size;
    uint32_t mp_obj_size;
    uint32_t mp_num_obj_per_chunk;
    uint32_t mp_num_obj_max_total;
    psmi_memtype_t mp_memtype;

    SLIST_HEAD(, mpool_element)	    mp_head;
    struct mpool_element **	    mp_elm_vector;
    struct mpool_element **	    mp_elm_vector_free;
    non_empty_callback_fn_t	    mp_non_empty_cb;
    void *			    mp_non_empty_cb_context;

};

static int	psmi_mpool_allocate_chunk(mpool_t);

/**
 * psmi_mpool_create()
 *
 * Create a memory pool and allocates <num_obj_per_chunk> objects of size
 * <obj_size>.  If more memory is needed to accomodate mpool_get()
 * requests, the memory pool will allocate another chunk of
 * <num_obj_per_chunk> objects, until it reaches the maximum number of objects
 * it can allocate.
 *
 * <obj_size>		size of each individual object
 * <num_obj_per_chunk>	number of objects to allocate per chunk (power of two)
 * <num_obj_max_total>	total number of objects that may be allocated
 *			at any given time. Must be a power of two greater than
 *			<num_obj_per_chunk>.
 *
 * <flags>		flags to be applied on the memory pool (ie. memory
 *			alignment)
 *
 * <cb>			callback to be called when the memory pool has some
 *			free objects available again (after running out of them).
 * <context>		context pointer for the callback
 *
 * Return the mpool on success, NULL on failure.
 */
mpool_t
psmi_mpool_create(size_t obj_size, uint32_t num_obj_per_chunk,
    uint32_t num_obj_max_total, int flags, psmi_memtype_t statstype, 
    non_empty_callback_fn_t cb, void *context)
{
    mpool_t mp;
    int s;
    size_t hdr_size;

#ifdef PSM_VALGRIND
    /* For Valgrind we which to define a "redzone" before and after the
     * allocation block, so we also allocate a blank mpool_element 
     * at the end of the user's block */
#endif

    if (!PSMI_POWEROFTWO(num_obj_per_chunk) ||
	!PSMI_POWEROFTWO(num_obj_max_total) ||
	num_obj_max_total < num_obj_per_chunk) {
	return NULL;
    }

    mp = psmi_calloc(PSMI_EP_NONE, statstype, 1, sizeof(struct mpool));
    if (mp == NULL) {
	fprintf(stderr, "Failed to allocate memory for memory pool: %s\n",
	    strerror(errno));
	return NULL;
    }

    for (s = 1; s < num_obj_per_chunk; s <<= 1)
	   mp->mp_vector_shift++;

    mp->mp_flags = flags;
    mp->mp_num_obj_per_chunk = num_obj_per_chunk;
    mp->mp_num_obj_max_total = num_obj_max_total;
    mp->mp_non_empty_cb = cb;
    mp->mp_non_empty_cb_context = context;

    mp->mp_memtype = statstype;

    SLIST_INIT(&mp->mp_head);
    mp->mp_elm_vector_size = num_obj_max_total / num_obj_per_chunk;
    mp->mp_elm_vector = psmi_calloc(PSMI_EP_NONE, statstype, mp->mp_elm_vector_size,
				    sizeof(struct mpool_element *));
    if (mp->mp_elm_vector == NULL) {
	fprintf(stderr, "Failed to allocate memory for memory pool vector: "
	    "%s\n", strerror(errno));
	psmi_free(mp);
	return NULL;
    }

    mp->mp_elm_vector_free = mp->mp_elm_vector;

    if (flags & PSMI_MPOOL_ALIGN) {
	/* User wants its block to start on a PSMI_MPOOL_ALIGNMENT
	 * boundary. */
	hdr_size        = PSMI_ALIGNUP(sizeof(struct mpool_element), 
				       PSMI_MPOOL_ALIGNMENT);
	mp->mp_obj_size = PSMI_ALIGNUP(obj_size, PSMI_MPOOL_ALIGNMENT);
	mp->mp_elm_size = hdr_size + mp->mp_obj_size;

	mp->mp_elm_offset = hdr_size - sizeof(struct mpool_element);
    } else {
	hdr_size	= sizeof(struct mpool_element);
	mp->mp_obj_size = PSMI_ALIGNUP(obj_size, 8);
	mp->mp_elm_size = hdr_size + mp->mp_obj_size;
	mp->mp_elm_offset = 0;
    }

    if (psmi_mpool_allocate_chunk(mp) != PSM_OK) {
	psmi_mpool_destroy(mp);
	return NULL;
    }

    VALGRIND_CREATE_MEMPOOL(mp, 0 /* no redzone */, PSM_VALGRIND_MEM_UNDEFINED);

    return mp;
}

/**
 * psmi_mpool_get()
 *
 * <mp>	    memory pool
 *
 * Requests an object from the memory pool.
 *
 * Returns NULL if the maximum number of objects has been allocated (refer to
 * <num_obj_max_total> in psmi_mpool_create) or if running out of memory.
 */
void *
psmi_mpool_get(mpool_t mp)
{
    struct mpool_element *me;
    void *obj;

    if (SLIST_EMPTY(&mp->mp_head)) {
	if (psmi_mpool_allocate_chunk(mp) != PSM_OK)
	    return NULL;
    }

    me = SLIST_FIRST(&mp->mp_head);
    SLIST_REMOVE_HEAD(&mp->mp_head, me_next);

    psmi_assert(!me->me_isused);
    me_mark_used(me);

    /* store a backpointer to the memory pool */
    me->me_mpool = mp;
    mp->mp_num_obj_inuse++;
    psmi_assert(mp->mp_num_obj_inuse <= mp->mp_num_obj);

    obj = (void *) ((uintptr_t) me + sizeof(struct mpool_element));
    VALGRIND_MEMPOOL_ALLOC(mp, obj, mp->mp_obj_size);
    return obj;
}

/**
 * psmi_mpool_put()
 *
 * <obj>    object to return to the memory pool
 *
 * Returns an <obj> to the memory pool subsystem.  This object will be re-used
 * to fulfill new psmi_mpool_get() requests.
 */
void
psmi_mpool_put(void *obj)
{
    struct mpool_element *me;
    int was_empty;
    mpool_t mp;

    me = (struct mpool_element *)
	((uintptr_t) obj - sizeof(struct mpool_element));
    me->me_gen_count++;

    mp = me->me_mpool;

    psmi_assert(mp != NULL);
    psmi_assert(mp->mp_num_obj_inuse >= 0);
    psmi_assert(me->me_isused);
    me_mark_unused(me);

    was_empty = mp->mp_num_obj_inuse == mp->mp_num_obj_max_total;
    SLIST_INSERT_HEAD(&mp->mp_head, me, me_next);

    mp->mp_num_obj_inuse--;

    VALGRIND_MEMPOOL_FREE(mp, obj);

    /* tell the user that memory is available */
    if (mp->mp_non_empty_cb && was_empty)
	mp->mp_non_empty_cb(mp->mp_non_empty_cb_context);
}

/**
 * psmi_mpool_get_obj_index()
 *
 * <obj>    object in the memory pool
 *
 * Returns the index of the <obj> in the memory pool.
 */

int
psmi_mpool_get_obj_index(void *obj)
{
    struct mpool_element *me = (struct mpool_element *)
	((uintptr_t) obj - sizeof(struct mpool_element));

    return me->me_index;
}

/**
 * psmi_mpool_get_obj_gen_count()
 *
 * <obj>    object in the memory pool
 *
 * Returns the generation count of the <obj>.
 */
uint32_t
psmi_mpool_get_obj_gen_count(void *obj)
{
    struct mpool_element *me = (struct mpool_element *)
	((uintptr_t) obj - sizeof(struct mpool_element));

    return me->me_gen_count;
}

/**
 * psmi_mpool_get_obj_index_gen_count()
 *
 * <obj>    object in the memory pool
 *
 * Returns the index of the <obj> in <index>.
 * Returns the generation count of the <obj> in <gen_count>.
 */
int
psmi_mpool_get_obj_index_gen_count(void *obj, uint32_t *index,
    uint32_t *gen_count)
{
    struct mpool_element *me = (struct mpool_element *)
	((uintptr_t) obj - sizeof(struct mpool_element));

    *index = me->me_index;
    *gen_count = me->me_gen_count;
    return 0;
}

/**
 * psmi_mpool_find_obj_by_index()
 *
 * <mp>	    memory pool
 * <index>  index of the object
 *
 * Returns the object located at <index> in the memory pool or NULL if the
 * <index> is invalid.
 */
void *
psmi_mpool_find_obj_by_index(mpool_t mp, int index)
{
    struct mpool_element *me;

    if_pf (index < 0 || index >= mp->mp_num_obj)
	return NULL;

    me = (struct mpool_element *)
	  ((uintptr_t) mp->mp_elm_vector[index >> mp->mp_vector_shift] +
	  (index & (mp->mp_num_obj_per_chunk - 1)) * mp->mp_elm_size + 
	  mp->mp_elm_offset);

    /* If this mpool doesn't require generation counts, it's illegal to find a
     * freed object */
#ifdef PSM_DEBUG
    if (mp->mp_flags & PSMI_MPOOL_NOGENERATION)
	psmi_assert(!me->me_isused);
#endif

    return (void *)((uintptr_t) me + sizeof(struct mpool_element));
}

/**
 * psmi_mpool_destroy()
 *
 * <mp>	    memory pool
 *
 * Destroy a previously allocated memory pool and reclaim its associated
 * memory.  The behavior is undefined if some objects have not been returned
 * to the memory pool with psmi_mpool_put().
 */
void
psmi_mpool_destroy(mpool_t mp)
{
    int i = 0;
    size_t nbytes = mp->mp_num_obj * mp->mp_elm_size;

    for (i = 0; i < mp->mp_elm_vector_size; i++) {
	if (mp->mp_elm_vector[i]) 
	    psmi_free(mp->mp_elm_vector[i]);
    }
    psmi_free(mp->mp_elm_vector);
    nbytes += mp->mp_elm_vector_size * sizeof(struct mpool_element *);
    VALGRIND_DESTROY_MEMPOOL(mp);
    psmi_free(mp);
    nbytes += sizeof(struct mpool);
}

/**
 * psmi_mpool_get_max_obj()
 *
 * <mp>	    memory pool
 *
 * Returns the num-obj-per-chunk
 * Returns the num-obj-max-total
 */
void		
psmi_mpool_get_obj_info(mpool_t mp, uint32_t *num_obj_per_chunk, 
			uint32_t *num_obj_max_total)
{
    *num_obj_per_chunk = mp->mp_num_obj_per_chunk;
    *num_obj_max_total = mp->mp_num_obj_max_total;
    return;
}

static int
psmi_mpool_allocate_chunk(mpool_t mp)
{
    struct mpool_element *elm;
    void *chunk;
    uint32_t i = 0, num_to_allocate;

    num_to_allocate =
	mp->mp_num_obj + mp->mp_num_obj_per_chunk > mp->mp_num_obj_max_total ?
	0 : mp->mp_num_obj_per_chunk;

    psmi_assert(mp->mp_num_obj + mp->mp_num_obj_per_chunk <=
	mp->mp_num_obj_max_total);

    if (num_to_allocate == 0)
	return PSM_NO_MEMORY;

    chunk = psmi_malloc(PSMI_EP_NONE, mp->mp_memtype, 
			num_to_allocate * mp->mp_elm_size);
    if (chunk == NULL) {
	fprintf(stderr,
	    "Failed to allocate memory for memory pool chunk: %s\n",
	    strerror(errno));
	return PSM_NO_MEMORY;
    }

    for (i = 0; i < num_to_allocate; i++) {
	elm = (struct mpool_element *)((uintptr_t)chunk +
	    i * mp->mp_elm_size + mp->mp_elm_offset);
	elm->me_gen_count = 0;
	elm->me_index = mp->mp_num_obj + i;
#ifdef PSM_DEBUG
	elm->me_isused = 0;
#endif
	SLIST_INSERT_HEAD(&mp->mp_head, elm, me_next);
#if 0
	fprintf(stderr, "chunk%ld i=%d elm=%p user=%p next=%p\n",
	    (long)(mp->mp_elm_vector_free - mp->mp_elm_vector), (int) i, elm,
	    (void *)((uintptr_t) elm + sizeof(struct mpool_element)),
	    SLIST_NEXT(elm, me_next));
#endif
    }

    psmi_assert((uintptr_t) mp->mp_elm_vector_free
	< ((uintptr_t) mp->mp_elm_vector) + mp->mp_elm_vector_size
	* sizeof(struct mpool_element *));

    mp->mp_elm_vector_free[0] = chunk;
    mp->mp_elm_vector_free++;
    mp->mp_num_obj += num_to_allocate;

    return PSM_OK;
}

#if 0
void
psmi_mpool_dump(mpool_t mp)
{
    int i, j;
    struct mpool_element *me;

    fprintf(stderr, "Memory pool %p has %d elements per chunk.\n",
	mp, mp->mp_num_obj_per_chunk);
    for (i = 0; i < mp->mp_elm_vector_size; i++) {
	if (mp->mp_elm_vector[i] != NULL) {
	    fprintf(stderr, "===========================\n");
	    fprintf(stderr, "mpool chunk #%d\n", i);

	    for (j = 0, me = mp->mp_elm_vector[i];
		j < mp->mp_num_obj_per_chunk;
		j++, me = (struct mpool_element *)
		    ((uintptr_t) me + mp->mp_elm_size)) {
		fprintf(stderr, "obj=%p index=%d gen_count=%d\n",
		    (void *) ((uintptr_t) me + sizeof(struct mpool_element)),
		    me->me_index, me->me_gen_count);
	    }
	    fprintf(stderr, "===========================\n");
	}
    }
}
#endif
