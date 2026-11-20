#ifndef FLOWIE_CONTROL_STORE_SCHEMA_INTERNAL_H
#define FLOWIE_CONTROL_STORE_SCHEMA_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLOWIE_CONTROL_TURBODB_SCHEMA_VERSION 7
#define FLOWIE_CONTROL_TURBODB_SCHEMA_FINGERPRINT \
  "flowie-control-persistent-session-schema-v7-20260829"

/** Return the immutable generated DDL for a supported TurboDB SQL driver. */
const char *flowie_control_store_schema_sql(const char *driver, size_t *size_out);

/** Generated dialect-specific entry points used by the dispatcher. */
const char *flowie_control_store_schema_sqlite_sql(size_t *size_out);
const char *flowie_control_store_schema_postgresql_sql(size_t *size_out);

#ifdef __cplusplus
}
#endif

#endif
