#ifndef FLOWIE_ORM_FLOW_INTERNAL_H
#define FLOWIE_ORM_FLOW_INTERNAL_H

#include "orm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FLOWIE_ORM_MAX_COLUMNS = 24u };

typedef enum flowie_orm_column_kind_e {
  FLOWIE_ORM_COLUMN_UINT64,
  FLOWIE_ORM_COLUMN_INT64,
  FLOWIE_ORM_COLUMN_TEXT,
  FLOWIE_ORM_COLUMN_BLOB
} flowie_orm_column_kind_t;

typedef struct flowie_orm_column_s {
  const char *name;
  flowie_orm_column_kind_t kind;
} flowie_orm_column_t;

typedef struct flowie_orm_row_s {
  uint64_t unsigned_values[FLOWIE_ORM_MAX_COLUMNS];
  int64_t signed_values[FLOWIE_ORM_MAX_COLUMNS];
  tstr buffers[FLOWIE_ORM_MAX_COLUMNS];
} flowie_orm_row_t;

/* The row and every returned buffer are borrowed only for the callback. */
typedef int (*flowie_orm_row_visit_fn)(void *ctx, const flowie_orm_row_t *row,
                                      size_t row_index);

orm_status_t flowie_orm_connect(const orm_config_t *config,
                                orm_connection_t **out_connection,
                                orm_error_t *error);
int flowie_orm_status_to_salts(orm_status_t status);
int flowie_orm_query_visit(orm_query_t *query, orm_transaction_t *transaction,
                           const flowie_orm_column_t *columns, size_t column_count,
                           size_t max_rows, size_t max_buffer_bytes,
                           flowie_orm_row_visit_fn visit, void *visit_ctx,
                           size_t *row_count);
int flowie_orm_command_execute(orm_query_t *query, orm_transaction_t *transaction,
                               uint64_t *affected_rows);

uint64_t flowie_orm_row_uint64(const flowie_orm_row_t *row, size_t column);
int64_t flowie_orm_row_int64(const flowie_orm_row_t *row, size_t column);
tstr flowie_orm_row_buffer(const flowie_orm_row_t *row, size_t column);

#ifdef __cplusplus
}
#endif

#endif
