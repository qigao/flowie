#ifndef FLOWIE_FACT_RECORD_INTERNAL_H
#define FLOWIE_FACT_RECORD_INTERNAL_H

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#define FLOWIE_RECORD_REVISION_ABSENT 0u
#define FLOWIE_RECORD_REVISION_MAX INT64_MAX

typedef struct flowie_record_view_s {
  size_t size;
  const uint8_t *key;
  size_t key_size;
  uint64_t revision;
  const uint8_t *value;
  size_t value_size;
} flowie_record_view_t;

#define FLOWIE_RECORD_VIEW_INIT {sizeof(flowie_record_view_t), NULL, 0u, 0u, NULL, 0u}

typedef int (*flowie_record_visit_fn)(void *ctx, const flowie_record_view_t *record);

typedef enum flowie_record_mutation_kind_e {
  FLOWIE_RECORD_PUT = 1,
  FLOWIE_RECORD_DELETE
} flowie_record_mutation_kind_t;

typedef struct flowie_record_mutation_s {
  size_t size;
  flowie_record_mutation_kind_t kind;
  const uint8_t *key;
  size_t key_size;
  uint64_t expected_revision;
  uint64_t next_revision;
  const uint8_t *value;
  size_t value_size;
} flowie_record_mutation_t;

#define FLOWIE_RECORD_MUTATION_INIT                                                               \
  {sizeof(flowie_record_mutation_t), FLOWIE_RECORD_PUT, NULL, 0u, 0u, 0u, NULL, 0u}

#endif
