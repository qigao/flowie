/* External installed-SDK consumer: real TurboDB PostgreSQL Plugin admission.
 *
 * No source-tree targets, driver registry fallback, or test-only private
 * Flowie/Salt APIs. The absolute Plugin path and two trusted dependency
 * directories originate from the SAME released/installed SDK graph.
 * The test does not require a live database and does not mask load failure.
 */
#include <orm_runtime.h>
#include <orm.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#ifndef FLOWIE_TEST_INSTALLED_PG_PLUGIN
#error "Expected immutable installed TurboDB PostgreSQL Plugin path"
#endif
#ifndef FLOWIE_TEST_INSTALLED_TURBODB_BIN
#error "Expected immutable installed TurboDB runtime directory"
#endif
#ifndef FLOWIE_TEST_INSTALLED_SALTS_BIN
#error "Expected immutable installed Salts runtime directory"
#endif

static int admit_exact_directory(const char *utf8, DLL_DIRECTORY_COOKIE *out) {
  wchar_t path[2048];
  const int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      utf8, -1, path,
                                      (int)(sizeof(path) / sizeof(path[0])));
  if (len <= 0 || len >= (int)(sizeof(path) / sizeof(path[0])))
    return 0;
  *out = AddDllDirectory(path);
  return *out != NULL;
}

int main(void) {
  DLL_DIRECTORY_COOKIE bin_db = NULL;
  DLL_DIRECTORY_COOKIE bin_core = NULL;
  orm_runtime_config_t settings;
  orm_runtime_t *runtime = NULL;
  orm_driver_load_config_t load = {0};
  orm_driver_info_t info = {0};
  orm_error_t error;
  orm_status_t rc = ORM_STATUS_INVALID_ARGUMENT;
  int ok = 0;

  /* No directory wildcard, process PATH mutation or global
   * SetDefaultDllDirectories side effects. */
  if (!admit_exact_directory(FLOWIE_TEST_INSTALLED_TURBODB_BIN, &bin_db) ||
      !admit_exact_directory(FLOWIE_TEST_INSTALLED_SALTS_BIN, &bin_core)) {
    fprintf(stderr, "Cannot admit exact installed SDK DLL directories: Win32=%lu\n",
            (unsigned long)GetLastError());
    goto cleanup;
  }

  orm_runtime_config_init(&settings);
  orm_error_init(&error);
  rc = orm_runtime_create(&settings, &runtime, &error);
  if (rc != ORM_STATUS_OK || !runtime) {
    fprintf(stderr, "Installed ORM runtime create status=%d: %s\n",
            (int)rc, error.message);
    goto cleanup;
  }
  load.struct_size = (uint32_t)sizeof(load);
  load.abi_version = ORM_RUNTIME_ABI_VERSION;
  load.module_path = orm_view(FLOWIE_TEST_INSTALLED_PG_PLUGIN);
  load.expected_driver_id = orm_view("postgresql");
  rc = orm_runtime_load_driver(runtime, &load, &error);
  if (rc != ORM_STATUS_OK) {
    fprintf(stderr, "Installed PostgreSQL Plugin admission status=%d: %s\n",
            (int)rc, error.message);
    goto cleanup;
  }
  rc = orm_runtime_driver_info(runtime, orm_view("postgresql"), &info, &error);
  if (rc != ORM_STATUS_OK ||
      info.abi_version != ORM_RUNTIME_ABI_VERSION ||
      info.canonical_id_size != 10u ||
      memcmp(info.canonical_id, "postgresql", 10u) != 0) {
    fprintf(stderr, "Installed PostgreSQL Plugin identity failure status=%d: %s\n",
            (int)rc, error.message);
    goto cleanup;
  }
  ok = 1;
cleanup:
  if (runtime) {
    orm_error_init(&error);
    rc = orm_runtime_close(runtime, &error);
    if (rc != ORM_STATUS_OK) {
      fprintf(stderr, "Installed Plugin close failed status=%d: %s\n",
              (int)rc, error.message);
      ok = 0;
    }
    orm_runtime_release(runtime);
  }
  if (bin_core && !RemoveDllDirectory(bin_core)) ok = 0;
  if (bin_db && !RemoveDllDirectory(bin_db)) ok = 0;
  if (ok) puts("flowie.installed.postgresql_plugin: PASS exact CMeta Plugin ABI/ID and clean release");
  return ok ? 0 : 1;
}
