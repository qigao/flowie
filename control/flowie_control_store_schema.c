#include "flowie_control_store_schema_internal.h"

#include <string.h>

const char *flowie_control_store_schema_sql(const char *driver, size_t *size_out) {
  if (size_out) *size_out = 0u;
  if (!driver) return NULL;
  if (strcmp(driver, "sqlite") == 0)
    return flowie_control_store_schema_sqlite_sql(size_out);
  if (strcmp(driver, "postgresql") == 0)
    return flowie_control_store_schema_postgresql_sql(size_out);
  return NULL;
}
