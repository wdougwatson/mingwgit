/*
 * alloc.c  - specialized allocator for internal objects
 *
 * Copyright (C) 2006 Linus Torvalds
 *
 * The standard malloc/free wastes too much space for objects, partly because
 * it maintains all the allocation infrastructure (which isn't needed, since
 * we never free an object descriptor anyway), but even more because it ends
 * up with maximal alignment because it doesn't know what the object alignment
 * for the new allocation is.
 */
#include "cache.h"
#include "object.h"
#include "blob.h"
#include "tree.h"
#include "commit.h"
#include "tag.h"

#define BLOCKING 1024

#define DEFINE_ALLOCATOR(name, type)				\
static unsigned int name##_allocs;				\
void *alloc_##name##_node(void)					\
{								\
	static int nr;						\
	static type *block;					\
	void *ret;						\
								\
	if (!nr) {						\
		nr = BLOCKING;					\
		block = xmalloc(BLOCKING * sizeof(type));	\
	}							\
	nr--;							\
	name##_allocs++;					\
	ret = block++;						\
	memset(ret, 0, sizeof(type));				\
	return ret;						\
}

union any_object {
	struct object object;
	struct blob blob;
	struct tree tree;
	struct commit commit;
	struct tag tag;
};

DEFINE_ALLOCATOR(blob, struct blob)
DEFINE_ALLOCATOR(tree, struct tree)
DEFINE_ALLOCATOR(raw_commit, struct commit)
DEFINE_ALLOCATOR(tag, struct tag)
DEFINE_ALLOCATOR(object, union any_object)

void *alloc_commit_node(void)
{
	static int commit_count;
	struct commit *c = alloc_raw_commit_node();
	c->index = commit_count++;
	return c;
}

static void report(const char *name, unsigned int count, size_t size)
{
	fprintf(stderr, "%10s: %8u (%"PRIuMAX" kB)\n",
			name, count, (uintmax_t) size);
}

#define REPORT(name, type)	\
    report(#name, name##_allocs, name##_allocs * sizeof(type) >> 10)

void alloc_report(void)
{
	REPORT(blob, struct blob);
	REPORT(tree, struct tree);
	REPORT(raw_commit, struct commit);
	REPORT(tag, struct tag);
	REPORT(object, union any_object);
}
