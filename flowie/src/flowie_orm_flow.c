#include "flowie_orm_flow_internal.h"

#include "orm_runtime.h"

#include "cmeta_cmeta_data.h"
#include "cmeta_error.h"
#include "cmeta_fs.h"
#include <salts/thread.h>

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
static cmeta_data_desc flowie_orm_text_data;
static cmeta_data_desc flowie_orm_blob_data;
static cmeta_once_t flowie_orm_data_once = SALTS_ONCE_INIT;

static void flowie_orm_data_init(void) {
  flowie_orm_text_data = cmeta_tstr_cmeta_data;
  flowie_orm_text_data.stable_id = "flowie.orm.Text";
  flowie_orm_text_data.display_name = "Flowie ORM text";
  flowie_orm_text_data.kind = CMETA_DATA_STRING;
  flowie_orm_blob_data = cmeta_tstr_cmeta_data;
  flowie_orm_blob_data.stable_id = "flowie.orm.Blob";
  flowie_orm_blob_data.display_name = "Flowie ORM blob";
  flowie_orm_blob_data.kind = CMETA_DATA_BYTES;
}

static int flowie_orm_view_is(orm_string_view_t driver, const char *name) {
  const size_t name_size = name ? strlen(name) : 0u;
  return name_size != 0u && driver.len == name_size && driver.data != NULL &&
         memcmp(driver.data, name, name_size) == 0;
}

static orm_status_t flowie_orm_error(orm_error_t *error, orm_status_t status) {
  if (error) {
    const char *message = orm_status_message(status);
    const size_t length = strlen(message);
    orm_error_init(error);
    error->status = status;
    /* Copy the public status text into the fixed-size TurboDB ABI carrier. */
    memcpy(error->message, message,
           length < sizeof(error->message) ? length : sizeof(error->message) - 1u);
  }
  return status;
}

static const char *flowie_orm_default_module(orm_string_view_t driver) {
  if (flowie_orm_view_is(driver, "sqlite"))
    return FLOWIE_ORM_DRIVER_DIR "/turbodb_driver_sqlite" FLOWIE_ORM_MODULE_SUFFIX;
  if (flowie_orm_view_is(driver, "postgresql"))
    return FLOWIE_ORM_DRIVER_DIR "/turbodb_driver_postgresql" FLOWIE_ORM_MODULE_SUFFIX;
  if (flowie_orm_view_is(driver, "mysql"))
    return FLOWIE_ORM_DRIVER_DIR "/turbodb_driver_mysql" FLOWIE_ORM_MODULE_SUFFIX;
  if (flowie_orm_view_is(driver, "tidesdb"))
    return FLOWIE_ORM_DRIVER_DIR "/turbodb_driver_tidesdb" FLOWIE_ORM_MODULE_SUFFIX;
  return NULL;
}

orm_status_t flowie_orm_connect(const orm_config_t *config,
                                orm_connection_t **out_connection,
                                orm_error_t *error) {
  orm_runtime_config_t runtime_config;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load = {0};
  orm_config_t database;
  orm_option_t options[FLOWIE_ORM_MAX_OPTIONS];
  orm_status_t status;
  int explicit_module = 0;
  if (out_connection) *out_connection = NULL;
  if (!config || !out_connection)
    return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
  if (config->struct_size != sizeof(*config) || config->abi_version != ORM_C_ABI_VERSION)
    return flowie_orm_error(error, ORM_STATUS_ABI_MISMATCH);
  if (!config->driver.data || !config->driver.len ||
      (config->option_count && !config->options))
    return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
  if (config->option_count > FLOWIE_ORM_MAX_OPTIONS)
    return flowie_orm_error(error, ORM_STATUS_LIMIT_EXCEEDED);
  database = *config;
  database.options = options;
  database.option_count = 0u;
  if (flowie_orm_view_is(database.driver, "postgres")) database.driver = orm_view("postgresql");
  for (uint32_t index = 0u; index < config->option_count; ++index) {
    const orm_option_t *option = &config->options[index];
    if (!option->keyword.data || !option->keyword.len ||
        (option->value.len && !option->value.data))
      return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
    if (flowie_orm_view_is(option->keyword, FLOWIE_ORM_DRIVER_MODULE_OPTION)) {
      if (explicit_module || !option->value.len)
        return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
      explicit_module = 1;
      load.module_path = option->value;
    } else {
      options[database.option_count++] = *option;
    }
  }
  if (!explicit_module) {
    const char *path = flowie_orm_default_module(database.driver);
    if (!path) return flowie_orm_error(error, ORM_STATUS_DRIVER_NOT_REGISTERED);
    load.module_path = orm_view(path);
  } else {
    tstr path;
    int absolute;
    if (load.module_path.len > ORM_RUNTIME_DEFAULT_MAX_MODULE_PATH_BYTES ||
        memchr(load.module_path.data, '\0', load.module_path.len))
      return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
    path = tstr_from_v(load.module_path);
    if (!path) return flowie_orm_error(error, ORM_STATUS_OUT_OF_MEMORY);
    absolute = cmeta_fs_path_is_absolute(path);
    tstr_freep(&path);
    if (!absolute) return flowie_orm_error(error, ORM_STATUS_INVALID_ARGUMENT);
  }
  load.struct_size = sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.expected_driver_id = database.driver;
  orm_runtime_config_init(&runtime_config);
  runtime_config.max_drivers = 1u;
  runtime_config.max_connections = 1u;
  runtime_config.max_pending_operations = 1u;
  status = orm_runtime_create(&runtime_config, &runtime, error);
  if (status == ORM_STATUS_OK) status = orm_runtime_load_driver(runtime, &load, error);
  if (status == ORM_STATUS_OK) status = orm_runtime_connect(runtime, &database, out_connection, error);
  /* A successful connection holds the runtime/Plugin lease until final release.
   * On failure this closes the runtime; release does not overwrite the error. */
  orm_runtime_release(runtime);
  return status;
}

int flowie_orm_status_to_salts(orm_status_t status) {
  switch (status) {
    case ORM_STATUS_OK: return SALTS_OK;
    case ORM_STATUS_INVALID_ARGUMENT:
    case ORM_STATUS_ABI_MISMATCH:
    case ORM_STATUS_TYPE_ERROR:
    case ORM_STATUS_OUT_OF_RANGE:
    case ORM_STATUS_NULL_VALUE:
    case ORM_STATUS_DRIVER_ID_MISMATCH:
    case ORM_STATUS_INVALID_STATE: return SALTS_EINVAL;
    case ORM_STATUS_OUT_OF_MEMORY: return SALTS_ENOMEM;
    case ORM_STATUS_LIMIT_EXCEEDED: return SALTS_ENOSPC;
    case ORM_STATUS_BUSY: return SALTS_EBUSY;
    case ORM_STATUS_UNSUPPORTED: return SALTS_ENOTSUP;
    case ORM_STATUS_DRIVER_NOT_REGISTERED:
    case ORM_STATUS_DRIVER_MODULE_NOT_FOUND: return SALTS_ENOENT;
    case ORM_STATUS_CONNECTION_ERROR:
    case ORM_STATUS_SQL_ERROR:
    case ORM_STATUS_DATASTORE_ERROR:
    case ORM_STATUS_INTERNAL_ERROR:
    default: return SALTS_EIO;
  }
}

static int flowie_orm_column_metadata(const flowie_orm_column_t *column, size_t index,
                                      cmeta_field_desc *layout,
                                      cmeta_data_field_desc *field) {
  const cmeta_data_desc *data = NULL;
  size_t offset = 0u;
  if (!column || !column->name || !column->name[0] || !layout || !field)
    return SALTS_EINVAL;
  cmeta_once(&flowie_orm_data_once, flowie_orm_data_init);
  switch (column->kind) {
    case FLOWIE_ORM_COLUMN_UINT64:
      data = &cmeta_data_uint64;
      offset = offsetof(flowie_orm_row_t, unsigned_values) + index * sizeof(uint64_t);
      break;
    case FLOWIE_ORM_COLUMN_INT64:
      data = &cmeta_data_int64;
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
    default: return SALTS_EINVAL;
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
  return SALTS_OK;
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
  int rc = SALTS_OK;
  if (row_count) *row_count = 0u;
  if (!query || !columns || column_count == 0u ||
      column_count > FLOWIE_ORM_MAX_COLUMNS || max_rows == 0u || !visit)
    return SALTS_EINVAL;
  memset(layout_fields, 0, sizeof(layout_fields));
  memset(data_fields, 0, sizeof(data_fields));
  for (index = 0u; index < column_count; ++index) {
    rc = flowie_orm_column_metadata(&columns[index], index, &layout_fields[index],
                                    &data_fields[index]);
    if (rc != SALTS_OK) return rc;
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
  rc = flowie_orm_status_to_salts(status);
  if (rc != SALTS_OK) return rc;
  memset(&row, 0, sizeof(row));
  for (;;) {
    cflow_step step = cflow_publisher_resume(&publisher, NULL, &row);
    if (step.kind == CFLOW_STEP_VALUE || step.kind == CFLOW_STEP_VALUE_AND_DONE) {
      if (rows >= max_rows)
        rc = SALTS_ENOSPC;
      else
        rc = visit(visit_ctx, &row, rows);
      flowie_orm_row_destroy(&row);
      if (rc != SALTS_OK) break;
      ++rows;
      if (step.kind == CFLOW_STEP_VALUE_AND_DONE) break;
      continue;
    }
    if (step.kind == CFLOW_STEP_DONE) break;
    rc = step.kind == CFLOW_STEP_WAIT ? SALTS_ENOTSUP : SALTS_EIO;
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
  if (!query) return SALTS_EINVAL;
  orm_error_init(&error);
  status = transaction ? orm_query_open_command_flow_in_transaction(
                             query, transaction, &publisher, &error)
                       : orm_query_open_command_flow(query, &publisher, &error);
  rc = flowie_orm_status_to_salts(status);
  if (rc != SALTS_OK) return rc;
  step = cflow_publisher_resume(&publisher, NULL, &result);
  if (step.kind != CFLOW_STEP_VALUE_AND_DONE)
    rc = step.kind == CFLOW_STEP_WAIT ? SALTS_ENOTSUP : SALTS_EIO;
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
