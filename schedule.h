#ifndef _SCHEDULE_H
#define _SCHEDULE_H

/* An access to an array element or an iterator.
 * Accesses to iterators have an access relation that maps to an unnamed space.
 * An access may be both read and write.
 */
struct gpu_stmt_access {
	/* Access reads elements */
	int read;
	/* Access writes elements */
	int write;

	/* Index of the array reference group this reference belong to. */
	int group;

	/* Access relation */
	isl_map *access;
	/* The reference id of the corresponding pet_expr. */
	isl_id *ref_id;

	struct gpu_stmt_access *next;
};

struct gpu_stmt {
	isl_id *id;
	struct pet_stmt *stmt;

	/* Number of tile dimensions. */
	int tile_len;
	/* Number of initial parallel loops among tile dimensions. */
	int n_parallel;

	/* Linked list of accesses. */
	struct gpu_stmt_access *accesses;
};

__isl_give isl_map *project_out(__isl_take isl_space *dim,
	int len, int first, int n);
__isl_give isl_map *projection(__isl_take isl_space *dim,
	int src_len, int dst_len);
__isl_give isl_set *extend(__isl_take isl_set *set, int dst_len);
__isl_give isl_union_map *align_range(__isl_take isl_union_map *umap);

#endif
