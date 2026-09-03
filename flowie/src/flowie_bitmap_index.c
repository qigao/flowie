#include "flowie_bitmap_index_internal.h"

#include "roaring.h"
#include "salts_error.h"

#include <stdlib.h>

struct flowie_bitmap_index_s {
  roaring64_bitmap_t *bitmap;
  size_t max_members;
};

int flowie_bitmap_index_create(size_t max_members, flowie_bitmap_index_t **out) {
  flowie_bitmap_index_t *index;
  if (!out || max_members == 0u) return SALTS_EINVAL;
  *out = NULL;
  index = (flowie_bitmap_index_t *)calloc(1u, sizeof(*index));
  if (!index) return SALTS_ENOMEM;
  index->bitmap = roaring64_bitmap_create();
  if (!index->bitmap) {
    free(index);
    return SALTS_ENOMEM;
  }
  index->max_members = max_members;
  *out = index;
  return SALTS_OK;
}

void flowie_bitmap_index_destroy(flowie_bitmap_index_t *index) {
  if (!index) return;
  roaring64_bitmap_free(index->bitmap);
  free(index);
}

int flowie_bitmap_index_add(flowie_bitmap_index_t *index, uint64_t member) {
  if (!index || !index->bitmap) return SALTS_EINVAL;
  if (roaring64_bitmap_contains(index->bitmap, member)) return SALTS_OK;
  if (roaring64_bitmap_get_cardinality(index->bitmap) >= (uint64_t)index->max_members)
    return SALTS_ENOSPC;
  roaring64_bitmap_add(index->bitmap, member);
  return roaring64_bitmap_contains(index->bitmap, member) ? SALTS_OK : SALTS_ENOMEM;
}

int flowie_bitmap_index_remove(flowie_bitmap_index_t *index, uint64_t member) {
  if (!index || !index->bitmap) return SALTS_EINVAL;
  roaring64_bitmap_remove(index->bitmap, member);
  return SALTS_OK;
}

int flowie_bitmap_index_contains(const flowie_bitmap_index_t *index, uint64_t member,
                                     int *out) {
  if (!index || !index->bitmap || !out) return SALTS_EINVAL;
  *out = roaring64_bitmap_contains(index->bitmap, member) ? 1 : 0;
  return SALTS_OK;
}

int flowie_bitmap_index_count(const flowie_bitmap_index_t *index, size_t *out) {
  uint64_t count;
  if (!index || !index->bitmap || !out) return SALTS_EINVAL;
  count = roaring64_bitmap_get_cardinality(index->bitmap);
  if (count > (uint64_t)SIZE_MAX) return SALTS_ERANGE;
  *out = (size_t)count;
  return SALTS_OK;
}

int flowie_bitmap_index_select(const flowie_bitmap_index_t *index, size_t rank,
                                   uint64_t *out) {
  if (!index || !index->bitmap || !out) return SALTS_EINVAL;
  return roaring64_bitmap_select(index->bitmap, (uint64_t)rank, out) ? SALTS_OK : SALTS_ERANGE;
}
