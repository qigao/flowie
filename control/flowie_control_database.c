#include "flowie_control_database_internal.h"

#include <orm.h>
#include <turbo_str.h>

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct flowie_control_database_s {
  orm_connection_t *connection;
  orm_transaction_t *transaction;
  orm_error_t error;
  int last_status;
  int changes;
};

struct flowie_control_statement_s {
  flowie_control_database_t *database;
  tstr sql;
  orm_query_t *query;
  orm_result_t *result;
  uint64_t row_count;
  uint64_t row_position;
  uint64_t column_count;
  uint32_t binding_count;
  int executed;
};

static int flowie_control_database_map_status(orm_status_t status) {
  switch (status) {
  case ORM_STATUS_OK:
    return FLOWIE_CONTROL_DB_OK;
  case ORM_STATUS_BUSY:
    return FLOWIE_CONTROL_DB_BUSY;
  case ORM_STATUS_CONSTRAINT:
    return FLOWIE_CONTROL_DB_CONSTRAINT;
  case ORM_STATUS_OUT_OF_MEMORY:
    return FLOWIE_CONTROL_DB_NOMEM;
  case ORM_STATUS_OUT_OF_RANGE:
  case ORM_STATUS_LIMIT_EXCEEDED:
    return FLOWIE_CONTROL_DB_RANGE;
  case ORM_STATUS_INVALID_ARGUMENT:
  case ORM_STATUS_TYPE_ERROR:
  case ORM_STATUS_NULL_VALUE:
    return FLOWIE_CONTROL_DB_MISMATCH;
  default:
    return FLOWIE_CONTROL_DB_ERROR;
  }
}

static int flowie_control_database_record(flowie_control_database_t *database,
                                          orm_status_t status) {
  const int mapped = flowie_control_database_map_status(status);
  if (database != NULL) database->last_status = mapped;
  return mapped;
}

static int flowie_control_database_query_create(flowie_control_statement_t *statement) {
  const orm_status_t status =
      orm_raw(statement->database->connection, orm_view_tstr(statement->sql), &statement->query,
              &statement->database->error);
  return flowie_control_database_record(statement->database, status);
}

int flowie_control_database_open(const orm_config_t *config, flowie_control_database_t **out) {
  flowie_control_database_t *database;
  orm_status_t status;
  if (out != NULL) *out = NULL;
  if (config == NULL || out == NULL) return FLOWIE_CONTROL_DB_MISMATCH;
  database = (flowie_control_database_t *)calloc(1u, sizeof(*database));
  if (database == NULL) return FLOWIE_CONTROL_DB_NOMEM;
  orm_error_init(&database->error);
  status = orm_connect(config, &database->connection, &database->error);
  if (status != ORM_STATUS_OK) {
    const int mapped = flowie_control_database_record(database, status);
    free(database);
    return mapped;
  }
  database->last_status = FLOWIE_CONTROL_DB_OK;
  *out = database;
  return FLOWIE_CONTROL_DB_OK;
}

int flowie_control_database_close(flowie_control_database_t *database) {
  if (database == NULL) return FLOWIE_CONTROL_DB_OK;
  orm_transaction_destroy(database->transaction);
  orm_disconnect(database->connection);
  free(database);
  return FLOWIE_CONTROL_DB_OK;
}

int flowie_control_database_prepare(flowie_control_database_t *database, const char *sql,
                                    int sql_size, flowie_control_statement_t **out,
                                    const char **tail) {
  flowie_control_statement_t *statement;
  size_t size;
  int status;
  if (out != NULL) *out = NULL;
  if (tail != NULL) *tail = NULL;
  if (database == NULL || sql == NULL || out == NULL || sql_size < -1)
    return FLOWIE_CONTROL_DB_MISMATCH;
  size = sql_size < 0 ? strlen(sql) : (size_t)sql_size;
  if (size == 0u || memchr(sql, '\0', size) != NULL) return FLOWIE_CONTROL_DB_MISMATCH;
  statement = (flowie_control_statement_t *)calloc(1u, sizeof(*statement));
  if (statement == NULL) return flowie_control_database_record(database, ORM_STATUS_OUT_OF_MEMORY);
  statement->database = database;
  statement->sql = tstr_new_len(sql, size);
  if (statement->sql == NULL) {
    free(statement);
    return flowie_control_database_record(database, ORM_STATUS_OUT_OF_MEMORY);
  }
  status = flowie_control_database_query_create(statement);
  if (status != FLOWIE_CONTROL_DB_OK) {
    tstr_free(statement->sql);
    free(statement);
    return status;
  }
  *out = statement;
  if (tail != NULL) *tail = sql + size;
  return FLOWIE_CONTROL_DB_OK;
}

int flowie_control_database_finalize(flowie_control_statement_t *statement) {
  if (statement == NULL) return FLOWIE_CONTROL_DB_OK;
  orm_result_destroy(statement->result);
  orm_query_destroy(statement->query);
  tstr_free(statement->sql);
  free(statement);
  return FLOWIE_CONTROL_DB_OK;
}

static int flowie_control_database_bind(flowie_control_statement_t *statement, int index,
                                        orm_value_t value) {
  orm_status_t status;
  if (statement == NULL || statement->executed || index <= 0 ||
      (uint32_t)index != statement->binding_count + 1u)
    return FLOWIE_CONTROL_DB_MISMATCH;
  status = orm_query_bind(statement->query, value, &statement->database->error);
  if (status == ORM_STATUS_OK) ++statement->binding_count;
  return flowie_control_database_record(statement->database, status);
}

int flowie_control_database_bind_int64(flowie_control_statement_t *statement, int index,
                                       int64_t value) {
  return flowie_control_database_bind(statement, index, orm_i64(value));
}

int flowie_control_database_bind_int(flowie_control_statement_t *statement, int index, int value) {
  return flowie_control_database_bind_int64(statement, index, (int64_t)value);
}

int flowie_control_database_bind_text(flowie_control_statement_t *statement, int index,
                                      const char *value, int value_size,
                                      flowie_control_database_destructor_fn destructor) {
  orm_value_t input;
  size_t size;
  if (value == NULL || value_size < -1 ||
      (destructor != NULL && destructor != FLOWIE_CONTROL_DB_TRANSIENT))
    return FLOWIE_CONTROL_DB_MISMATCH;
  size = value_size < 0 ? strlen(value) : (size_t)value_size;
  input = orm_text_v((orm_string_view_t){value, size});
  return flowie_control_database_bind(statement, index, input);
}

int flowie_control_database_bind_blob(flowie_control_statement_t *statement, int index,
                                      const void *value, int value_size,
                                      flowie_control_database_destructor_fn destructor) {
  if (value_size < 0 || (value == NULL && value_size != 0) ||
      (destructor != NULL && destructor != FLOWIE_CONTROL_DB_TRANSIENT))
    return FLOWIE_CONTROL_DB_MISMATCH;
  return flowie_control_database_bind(statement, index, orm_blob(value, (size_t)value_size));
}

int flowie_control_database_bind_null(flowie_control_statement_t *statement, int index) {
  return flowie_control_database_bind(statement, index, orm_null());
}

int flowie_control_database_step(flowie_control_statement_t *statement) {
  orm_status_t status;
  uint64_t affected = 0u;
  if (statement == NULL) return FLOWIE_CONTROL_DB_MISMATCH;
  if (!statement->executed) {
    status =
        statement->database->transaction != NULL
            ? orm_query_execute_in_transaction(statement->query, statement->database->transaction,
                                               &statement->result, &statement->database->error)
            : orm_query_execute(statement->query, &statement->result, &statement->database->error);
    if (status != ORM_STATUS_OK) return flowie_control_database_record(statement->database, status);
    statement->executed = 1;
    status =
        orm_result_row_count(statement->result, &statement->row_count, &statement->database->error);
    if (status == ORM_STATUS_OK)
      status = orm_result_column_count(statement->result, &statement->column_count,
                                       &statement->database->error);
    if (status == ORM_STATUS_OK)
      status = orm_result_affected_rows(statement->result, &affected, &statement->database->error);
    if (status != ORM_STATUS_OK) return flowie_control_database_record(statement->database, status);
    statement->database->changes = affected > (uint64_t)INT_MAX ? INT_MAX : (int)affected;
  }
  if (statement->row_position < statement->row_count) {
    ++statement->row_position;
    statement->database->last_status = FLOWIE_CONTROL_DB_ROW;
    return FLOWIE_CONTROL_DB_ROW;
  }
  statement->database->last_status = FLOWIE_CONTROL_DB_DONE;
  return FLOWIE_CONTROL_DB_DONE;
}

int flowie_control_database_reset(flowie_control_statement_t *statement) {
  int status;
  if (statement == NULL) return FLOWIE_CONTROL_DB_MISMATCH;
  orm_result_destroy(statement->result);
  orm_query_destroy(statement->query);
  statement->result = NULL;
  statement->query = NULL;
  statement->row_count = 0u;
  statement->row_position = 0u;
  statement->column_count = 0u;
  statement->binding_count = 0u;
  statement->executed = 0;
  status = flowie_control_database_query_create(statement);
  return status;
}

int flowie_control_database_clear_bindings(flowie_control_statement_t *statement) {
  return statement != NULL && statement->binding_count == 0u ? FLOWIE_CONTROL_DB_OK
                                                             : FLOWIE_CONTROL_DB_MISMATCH;
}

static int flowie_control_database_cell(const flowie_control_statement_t *statement, int column,
                                        uint64_t *row, uint64_t *column_index) {
  if (statement == NULL || statement->result == NULL || statement->row_position == 0u ||
      column < 0 || (uint64_t)column >= statement->column_count)
    return 0;
  *row = statement->row_position - 1u;
  *column_index = (uint64_t)column;
  return 1;
}

int flowie_control_database_column_type(const flowie_control_statement_t *statement, int column) {
  orm_value_kind_t kind;
  uint64_t row;
  uint64_t column_index;
  if (!flowie_control_database_cell(statement, column, &row, &column_index) ||
      orm_result_value_kind(statement->result, row, column_index, &kind, NULL) != ORM_STATUS_OK)
    return FLOWIE_CONTROL_DB_NULL;
  switch (kind) {
  case ORM_VALUE_INT64:
  case ORM_VALUE_UINT64:
  case ORM_VALUE_BOOLEAN:
    return FLOWIE_CONTROL_DB_INTEGER;
  case ORM_VALUE_DOUBLE:
    return FLOWIE_CONTROL_DB_FLOAT;
  case ORM_VALUE_TEXT:
    return FLOWIE_CONTROL_DB_TEXT;
  case ORM_VALUE_BLOB:
    return FLOWIE_CONTROL_DB_BLOB;
  default:
    return FLOWIE_CONTROL_DB_NULL;
  }
}

int64_t flowie_control_database_column_int64(const flowie_control_statement_t *statement,
                                             int column) {
  orm_value_kind_t kind;
  int64_t signed_value = 0;
  uint64_t unsigned_value = 0u;
  uint8_t boolean_value = 0u;
  uint64_t row;
  uint64_t column_index;
  if (!flowie_control_database_cell(statement, column, &row, &column_index) ||
      orm_result_value_kind(statement->result, row, column_index, &kind, NULL) != ORM_STATUS_OK)
    return 0;
  if (kind == ORM_VALUE_INT64 && orm_result_get_int64(statement->result, row, column_index,
                                                      &signed_value, NULL) == ORM_STATUS_OK)
    return signed_value;
  if (kind == ORM_VALUE_UINT64 &&
      orm_result_get_uint64(statement->result, row, column_index, &unsigned_value, NULL) ==
          ORM_STATUS_OK &&
      unsigned_value <= (uint64_t)INT64_MAX)
    return (int64_t)unsigned_value;
  if (kind == ORM_VALUE_BOOLEAN && orm_result_get_boolean(statement->result, row, column_index,
                                                          &boolean_value, NULL) == ORM_STATUS_OK)
    return (int64_t)boolean_value;
  return 0;
}

int flowie_control_database_column_int(const flowie_control_statement_t *statement, int column) {
  const int64_t value = flowie_control_database_column_int64(statement, column);
  return value < (int64_t)INT_MIN || value > (int64_t)INT_MAX ? 0 : (int)value;
}

const unsigned char *
flowie_control_database_column_text(const flowie_control_statement_t *statement, int column) {
  orm_string_view_t value = {0};
  uint64_t row;
  uint64_t column_index;
  if (!flowie_control_database_cell(statement, column, &row, &column_index) ||
      orm_result_get_text(statement->result, row, column_index, &value, NULL) != ORM_STATUS_OK)
    return NULL;
  return (const unsigned char *)value.data;
}

const void *flowie_control_database_column_blob(const flowie_control_statement_t *statement,
                                                int column) {
  orm_blob_t value = {0};
  uint64_t row;
  uint64_t column_index;
  if (!flowie_control_database_cell(statement, column, &row, &column_index) ||
      orm_result_get_blob(statement->result, row, column_index, &value, NULL) != ORM_STATUS_OK)
    return NULL;
  return value.data;
}

int flowie_control_database_column_bytes(const flowie_control_statement_t *statement, int column) {
  orm_value_kind_t kind;
  orm_string_view_t text = {0};
  orm_blob_t blob = {0};
  size_t size = 0u;
  uint64_t row;
  uint64_t column_index;
  if (!flowie_control_database_cell(statement, column, &row, &column_index) ||
      orm_result_value_kind(statement->result, row, column_index, &kind, NULL) != ORM_STATUS_OK)
    return 0;
  if (kind == ORM_VALUE_TEXT &&
      orm_result_get_text(statement->result, row, column_index, &text, NULL) == ORM_STATUS_OK)
    size = text.len;
  else if (kind == ORM_VALUE_BLOB &&
           orm_result_get_blob(statement->result, row, column_index, &blob, NULL) == ORM_STATUS_OK)
    size = blob.size;
  return size > (size_t)INT_MAX ? 0 : (int)size;
}

int flowie_control_database_changes(const flowie_control_database_t *database) {
  return database != NULL ? database->changes : 0;
}

int flowie_control_database_errcode(const flowie_control_database_t *database) {
  return database != NULL ? database->last_status : FLOWIE_CONTROL_DB_ERROR;
}

const char *flowie_control_database_errmsg(const flowie_control_database_t *database) {
  return database != NULL && database->error.message[0] != '\0' ? database->error.message
                                                                : "TurboDB control database error";
}

static int flowie_control_database_keyword(const char *sql, const char *keyword) {
  size_t index = 0u;
  while (isspace((unsigned char)*sql))
    ++sql;
  while (keyword[index] != '\0' && sql[index] != '\0' &&
         tolower((unsigned char)sql[index]) == tolower((unsigned char)keyword[index]))
    ++index;
  if (keyword[index] != '\0') return 0;
  return sql[index] == '\0' || sql[index] == ';' || isspace((unsigned char)sql[index]);
}

static int flowie_control_database_exec_one(flowie_control_database_t *database, const char *sql) {
  flowie_control_statement_t *statement = NULL;
  orm_status_t orm_status;
  int status;
  if (flowie_control_database_keyword(sql, "begin")) {
    if (database->transaction != NULL)
      return flowie_control_database_record(database, ORM_STATUS_INVALID_STATE);
    orm_status = orm_transaction_begin(database->connection, ORM_ISOLATION_SERIALIZABLE,
                                       &database->transaction, &database->error);
    return flowie_control_database_record(database, orm_status);
  }
  if (flowie_control_database_keyword(sql, "commit")) {
    if (database->transaction == NULL)
      return flowie_control_database_record(database, ORM_STATUS_INVALID_STATE);
    orm_status = orm_transaction_commit(database->transaction, &database->error);
    if (orm_status == ORM_STATUS_OK) {
      orm_transaction_destroy(database->transaction);
      database->transaction = NULL;
    }
    return flowie_control_database_record(database, orm_status);
  }
  if (flowie_control_database_keyword(sql, "rollback")) {
    if (database->transaction == NULL)
      return flowie_control_database_record(database, ORM_STATUS_INVALID_STATE);
    orm_status = orm_transaction_rollback(database->transaction, &database->error);
    if (orm_status == ORM_STATUS_OK) {
      orm_transaction_destroy(database->transaction);
      database->transaction = NULL;
    }
    return flowie_control_database_record(database, orm_status);
  }
  status = flowie_control_database_prepare(database, sql, -1, &statement, NULL);
  if (status != FLOWIE_CONTROL_DB_OK) return status;
  do {
    status = flowie_control_database_step(statement);
  } while (status == FLOWIE_CONTROL_DB_ROW);
  flowie_control_database_finalize(statement);
  return status == FLOWIE_CONTROL_DB_DONE ? FLOWIE_CONTROL_DB_OK : status;
}

int flowie_control_database_exec(flowie_control_database_t *database, const char *sql,
                                 void *callback, void *callback_context, char **error_message) {
  const char *begin;
  const char *cursor;
  int status = FLOWIE_CONTROL_DB_OK;
  if (error_message != NULL) *error_message = NULL;
  if (database == NULL || sql == NULL || callback != NULL || callback_context != NULL)
    return FLOWIE_CONTROL_DB_MISMATCH;
  begin = sql;
  cursor = sql;
  for (;;) {
    if (*cursor == ';' || *cursor == '\0') {
      const char *left = begin;
      const char *right = cursor;
      tstr statement;
      while (left != right && isspace((unsigned char)*left))
        ++left;
      while (right != left && isspace((unsigned char)right[-1]))
        --right;
      if (right != left) {
        statement = tstr_new_len(left, (size_t)(right - left));
        if (statement == NULL)
          return flowie_control_database_record(database, ORM_STATUS_OUT_OF_MEMORY);
        status = flowie_control_database_exec_one(database, statement);
        tstr_free(statement);
        if (status != FLOWIE_CONTROL_DB_OK) return status;
      }
      if (*cursor == '\0') break;
      begin = cursor + 1;
    }
    ++cursor;
  }
  return status;
}
