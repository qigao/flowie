#ifndef FLOWIE_CONTROL_DATA_SCHEMA_INTERNAL_H
#define FLOWIE_CONTROL_DATA_SCHEMA_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Return the immutable SQLite archive DDL embedded at build time. */
const char *flowie_control_data_schema_sql(size_t *size_out);

#ifdef __cplusplus
}
#endif

#endif
