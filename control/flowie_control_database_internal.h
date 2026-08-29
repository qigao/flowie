#ifndef FLOWIE_CONTROL_DATABASE_INTERNAL_H
#define FLOWIE_CONTROL_DATABASE_INTERNAL_H

#include "orm.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flowie_control_database_s flowie_control_database_t;
typedef struct flowie_control_statement_s flowie_control_statement_t;
typedef void (*flowie_control_database_destructor_fn)(void *);

#define FLOWIE_CONTROL_DB_TRANSIENT ((flowie_control_database_destructor_fn)(intptr_t)-1)

enum {
  FLOWIE_CONTROL_DB_OK = 0,
  FLOWIE_CONTROL_DB_ERROR = 1,
  FLOWIE_CONTROL_DB_BUSY = 5,
  FLOWIE_CONTROL_DB_LOCKED = 6,
  FLOWIE_CONTROL_DB_NOMEM = 7,
  FLOWIE_CONTROL_DB_CONSTRAINT = 19,
  FLOWIE_CONTROL_DB_MISMATCH = 20,
  FLOWIE_CONTROL_DB_RANGE = 25,
  FLOWIE_CONTROL_DB_ROW = 100,
  FLOWIE_CONTROL_DB_DONE = 101
};

enum {
  FLOWIE_CONTROL_DB_INTEGER = 1,
  FLOWIE_CONTROL_DB_FLOAT = 2,
  FLOWIE_CONTROL_DB_TEXT = 3,
  FLOWIE_CONTROL_DB_BLOB = 4,
  FLOWIE_CONTROL_DB_NULL = 5
};

int flowie_control_database_open(const orm_config_t *config, flowie_control_database_t **out);
int flowie_control_database_close(flowie_control_database_t *database);
int flowie_control_database_exec(flowie_control_database_t *database, const char *sql,
                                 void *callback, void *callback_context, char **error_message);
int flowie_control_database_prepare(flowie_control_database_t *database, const char *sql,
                                    int sql_size, flowie_control_statement_t **out,
                                    const char **tail);
int flowie_control_database_finalize(flowie_control_statement_t *statement);
int flowie_control_database_step(flowie_control_statement_t *statement);
int flowie_control_database_reset(flowie_control_statement_t *statement);
int flowie_control_database_clear_bindings(flowie_control_statement_t *statement);
int flowie_control_database_bind_int64(flowie_control_statement_t *statement, int index,
                                       int64_t value);
int flowie_control_database_bind_int(flowie_control_statement_t *statement, int index, int value);
int flowie_control_database_bind_text(flowie_control_statement_t *statement, int index,
                                      const char *value, int value_size,
                                      flowie_control_database_destructor_fn destructor);
int flowie_control_database_bind_blob(flowie_control_statement_t *statement, int index,
                                      const void *value, int value_size,
                                      flowie_control_database_destructor_fn destructor);
int flowie_control_database_bind_null(flowie_control_statement_t *statement, int index);
int flowie_control_database_column_type(const flowie_control_statement_t *statement, int column);
int64_t flowie_control_database_column_int64(const flowie_control_statement_t *statement,
                                             int column);
int flowie_control_database_column_int(const flowie_control_statement_t *statement, int column);
const unsigned char *
flowie_control_database_column_text(const flowie_control_statement_t *statement, int column);
const void *flowie_control_database_column_blob(const flowie_control_statement_t *statement,
                                                int column);
int flowie_control_database_column_bytes(const flowie_control_statement_t *statement, int column);
int flowie_control_database_changes(const flowie_control_database_t *database);
int flowie_control_database_errcode(const flowie_control_database_t *database);
const char *flowie_control_database_errmsg(const flowie_control_database_t *database);

#ifdef __cplusplus
}
#endif

#endif
