#include "flowie_orm_flow_internal.h"

#if defined(FLOWIE_ORM_WITH_POSTGRESQL)
#include "orm_postgresql.h"
#endif

#include "turbo_cmeta_data.h"
#include "turbo_cmeta_fixed_width.h"
#include "turbo_error.h"

#include <cmeta/data.h>
#include <cmeta/struct.h>

#include <stddef.h>
#include <string.h>

static void flowie_orm_row_destroy(void *value) {
  flowie_orm_row_t *row = (flowie_orm_row_t *)value;
  size_t index;
  if (!row) return;
  for (index = 0u; index < FLOWIE_ORM_MAX_COLUMNS; ++index)
    tstr_freep(&row->buffers[index]);
  memset(row, 0, sizeof(*row));
}

static const cmeta_type_traits flowie_orm_row_traits = {
    .flags = CMETA_TRAIT_DESTROY,
    .destroy = flowie_orm_row_destroy,
};
static const cmeta_type_identity flowie_orm_row_identity =
    CMETA_TYPE_ID_ATOM_INIT("flowie.orm.Row");
static const cmeta_type_desc flowie_orm_row_type = {
    .name = "flowie_orm_row_t",
    .size = sizeof(flowie_orm_row_t),
    .align = _Alignof(flowie_orm_row_t),
    .kind = CMETA_T_OBJECT,
    .traits = &flowie_orm_row_traits,
    .identity = &flowie_orm_row_identity,
};
static const cmeta_data_buffer_shape flowie_orm_owned_buffer_shape = {
    .ownership = CMETA_DATA_BUFFER_OWNED,
};
static const cmeta_data_desc flowie_orm_text_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "flowie.orm.Text",
    .display_name = "Flowie ORM text",
    .kind = CMETA_DATA_STRING,
    .storage_type = &turbo_tstr_cmeta_type,
    .shape = &flowie_orm_owned_buffer_shape,
    .buffer_ops = &turbo_tstr_cmeta_buffer_ops,
};
static const cmeta_data_desc flowie_orm_blob_data = {
    .struct_size = sizeof(cmeta_data_desc),
    .abi_version = CMETA_DATA_DESC_ABI_VERSION,
    .stable_id = "flowie.orm.Blob",
    .display_name = "Flowie ORM blob",
    .kind = CMETA_DATA_BYTES,
    .storage_type = &turbo_tstr_cmeta_type,
    .shape = &flowie_orm_owned_buffer_shape,
    .buffer_ops = &turbo_tstr_cmeta_buffer_ops,
};

static int flowie_orm_driver_is(orm_string_view_t driver, const char *name) {
  const size_t name_size = name ? strlen(name) : 0u;
  return name_size != 0u && driver.len == name_size && driver.data != NULL &&
         memcmp(driver.data, name, name_size) == 0;
}

orm_status_t flowie_orm_connect(const orm_config_t *config,
                                orm_connection_t **out_connection,
                                orm_error_t *error) {
#if defined(FLOWIE_ORM_WITH_POSTGRESQL)
  if (config != NULL &&
      (flowie_orm_driver_is(config->driver, "postgres") ||
       flowie_orm_driver_is(config->driver, "postgresql")))
    return orm_postgresql_connect(config, out_connection, error);
#endif
  return orm_connect(config, out_connection, error);
}

int flowie_orm_status_to_turbo(orm_status_t status) {
  switch (status) {
    case ORM_STATUS_OK: return TURBO_OK;
    case ORM_STATUS_INVALID_ARGUMENT:
    case ORM_STATUS_ABI_MISMATCH:
    case ORM_STATUS_TYPE_ERROR:
    case ORM_STATUS_OUT_OF_RANGE:
    case ORM_STATUS_NULL_VALUE:
    case ORM_STATUS_INVALID_STATE: return TURBO_EINVAL;
    case ORM_STATUS_OUT_OF_MEMORY: return TURBO_ENOMEM;
    case ORM_STATUS_LIMIT_EXCEEDED: return TURBO_ENOSPC;
    case ORM_STATUS_BUSY: return TURBO_EBUSY;
    case ORM_STATUS_UNSUPPORTED: return TURBO_ENOTSUP;
    case ORM_STATUS_CONNECTION_ERROR:
    case ORM_STATUS_SQL_ERROR:
    case ORM_STATUS_DATASTORE_ERROR:
    case ORM_STATUS_INTERNAL_ERROR:
    default: return TURBO_EIO;
  }
}

static int flowie_orm_column_metadata(const flowie_orm_column_t *column, size_t index,
                                      cmeta_field_desc *layout,
                                      cmeta_data_field_desc *field) {
  const cmeta_data_desc *data = NULL;
  size_t offset = 0u;
  if (!column || !column->name || !column->name[0] || !layout || !field)
    return TURBO_EINVAL;
  switch (column->kind) {
    case FLOWIE_ORM_COLUMN_UINT64:
      data = &turbo_uint64_cmeta_data;
      offset = offsetof(flowie_orm_row_t, unsigned_values) + index * sizeof(uint64_t);
      break;
    case FLOWIE_ORM_COLUMN_INT64:
      data = &turbo_int64_cmeta_data;
      offset = offsetof(flowie_orm_row_t, signed_values) + index * sizeof(int64_t);
      break;
    case FLOWIE_ORM_COLUMN_TEXT:
      data = &flowie_orm_text_data;
      offset = offsetof(flowie_orm_row_t, buffers) + index * sizeof(tstr);
      break;
    case FLOWIE_ORM_COLUMN_BLOB:
      data = &flowie_orm_blob_data;
      offset = offsetof(flowie_orm_row_t, buffers) + index * sizeof(tstr);
      break;
    default: return TURBO_EINVAL;
  }
  layout->name = column->name;
  layout->type_name = data->storage_type->name;
  layout->offset = offset;
  layout->size = data->storage_type->size;
  layout->align = data->storage_type->align;
  layout->type = data->storage_type;
  layout->declared_type = NULL;
  field->stable_id = column->name;
  field->name = column->name;
  field->offset = offset;
  field->value = data;
  return TURBO_OK;
}

int flowie_orm_query_visit(orm_query_t *query, orm_transaction_t *transaction,
                           const flowie_orm_column_t *columns, size_t column_count,
                           size_t max_rows, size_t max_buffer_bytes,
                           flowie_orm_row_visit_fn visit, void *visit_ctx,
                           size_t *row_count) {
  cmeta_field_desc layout_fields[FLOWIE_ORM_MAX_COLUMNS];
  cmeta_data_field_desc data_fields[FLOWIE_ORM_MAX_COLUMNS];
  cmeta_struct_desc layout;
  cmeta_data_struct_shape struct_shape;
  cmeta_data_desc row_data;
  orm_flow_config_t flow_config;
  orm_error_t error;
  cflow_publisher publisher = {0};
  flowie_orm_row_t row;
  orm_status_t status;
  size_t rows = 0u;
  size_t index;
  int rc = TURBO_OK;
  if (row_count) *row_count = 0u;
  if (!query || !columns || column_count == 0u ||
      column_count > FLOWIE_ORM_MAX_COLUMNS || max_rows == 0u || !visit)
    return TURBO_EINVAL;
  memset(layout_fields, 0, sizeof(layout_fields));
  memset(data_fields, 0, sizeof(data_fields));
  for (index = 0u; index < column_count; ++index) {
    rc = flowie_orm_column_metadata(&columns[index], index, &layout_fields[index],
                                    &data_fields[index]);
    if (rc != TURBO_OK) return rc;
  }
  layout.name = "flowie_orm_row_t";
  layout.size = sizeof(flowie_orm_row_t);
  layout.align = _Alignof(flowie_orm_row_t);
  layout.fields = layout_fields;
  layout.field_count = column_count;
  struct_shape.layout = &layout;
  struct_shape.fields = data_fields;
  struct_shape.field_count = column_count;
  memset(&row_data, 0, sizeof(row_data));
  row_data.struct_size = sizeof(row_data);
  row_data.abi_version = CMETA_DATA_DESC_ABI_VERSION;
  row_data.stable_id = "flowie.orm.Row.data";
  row_data.display_name = "Flowie ORM row";
  row_data.kind = CMETA_DATA_STRUCT;
  row_data.storage_type = &flowie_orm_row_type;
  row_data.shape = &struct_shape;
  orm_flow_config(&flow_config, &row_data);
  if (max_buffer_bytes != 0u) flow_config.max_buffer_bytes = max_buffer_bytes;
  orm_error_init(&error);
  status = transaction ? orm_query_open_flow_in_transaction(
                             query, transaction, &flow_config, &publisher, &error)
                       : orm_query_open_flow(query, &flow_config, &publisher, &error);
  rc = flowie_orm_status_to_turbo(status);
  if (rc != TURBO_OK) return rc;
  memset(&row, 0, sizeof(row));
  for (;;) {
    cflow_step step = cflow_publisher_resume(&publisher, NULL, &row);
    if (step.kind == CFLOW_STEP_VALUE || step.kind == CFLOW_STEP_VALUE_AND_DONE) {
      if (rows >= max_rows)
        rc = TURBO_ENOSPC;
      else
        rc = visit(visit_ctx, &row, rows);
      flowie_orm_row_destroy(&row);
      if (rc != TURBO_OK) break;
      ++rows;
      if (step.kind == CFLOW_STEP_VALUE_AND_DONE) break;
      continue;
    }
    if (step.kind == CFLOW_STEP_DONE) break;
    rc = step.kind == CFLOW_STEP_WAIT ? TURBO_ENOTSUP : TURBO_EIO;
    break;
  }
  cflow_publisher_destroy(&publisher);
  if (row_count) *row_count = rows;
  return rc;
}

int flowie_orm_command_execute(orm_query_t *query, orm_transaction_t *transaction,
                               uint64_t *affected_rows) {
  orm_error_t error;
  cflow_publisher publisher = {0};
  orm_command_result_t result = ORM_COMMAND_RESULT_INIT;
  orm_status_t status;
  cflow_step step;
  int rc;
  if (affected_rows) *affected_rows = 0u;
  if (!query) return TURBO_EINVAL;
  orm_error_init(&error);
  status = transaction ? orm_query_open_command_flow_in_transaction(
                             query, transaction, &publisher, &error)
                       : orm_query_open_command_flow(query, &publisher, &error);
  rc = flowie_orm_status_to_turbo(status);
  if (rc != TURBO_OK) return rc;
  step = cflow_publisher_resume(&publisher, NULL, &result);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE)
    rc = step.kind == CFLOW_STEP_WAIT ? TURBO_ENOTSUP : TURBO_EIO;
  else if (affected_rows)
    *affected_rows = result.affected_rows;
  cflow_publisher_destroy(&publisher);
  return rc;
}

uint64_t flowie_orm_row_uint64(const flowie_orm_row_t *row, size_t column) {
  return row && column < FLOWIE_ORM_MAX_COLUMNS ? row->unsigned_values[column] : 0u;
}

int64_t flowie_orm_row_int64(const flowie_orm_row_t *row, size_t column) {
  return row && column < FLOWIE_ORM_MAX_COLUMNS ? row->signed_values[column] : 0;
}

tstr flowie_orm_row_buffer(const flowie_orm_row_t *row, size_t column) {
  return row && column < FLOWIE_ORM_MAX_COLUMNS ? row->buffers[column] : NULL;
}
