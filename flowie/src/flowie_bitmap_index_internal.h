#ifndef FLOWIE_BITMAP_INDEX_H
#define FLOWIE_BITMAP_INDEX_H

#include "flowie_export.h"
#include "platform.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Derived uint64 membership index backed by CRoaring.
 *
 * This index is not a fact store: callers must be able to rebuild it from State/Index facts.
 * Instances have one mutable owner and are not thread-safe.
 */
typedef struct flowie_bitmap_index_s flowie_bitmap_index_t;

/** Create an index with a non-zero hard cardinality limit. */
FLOWIE_C_API int flowie_bitmap_index_create(size_t max_members,
                                             flowie_bitmap_index_t **out);
FLOWIE_C_API void flowie_bitmap_index_destroy(flowie_bitmap_index_t *index);

/** Idempotently add/remove one integer member. */
FLOWIE_C_API int flowie_bitmap_index_add(flowie_bitmap_index_t *index, uint64_t member);
FLOWIE_C_API int flowie_bitmap_index_remove(flowie_bitmap_index_t *index, uint64_t member);

FLOWIE_C_API int flowie_bitmap_index_contains(const flowie_bitmap_index_t *index,
                                                uint64_t member, int *out);
FLOWIE_C_API int flowie_bitmap_index_count(const flowie_bitmap_index_t *index, size_t *out);

/** Select the zero-based member rank in ascending order; an invalid rank returns TURBO_ERANGE. */
FLOWIE_C_API int flowie_bitmap_index_select(const flowie_bitmap_index_t *index, size_t rank,
                                              uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif
