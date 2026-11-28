#ifndef FLOWIE_RULESFORGE_PACKAGE_INTERNAL_H
#define FLOWIE_RULESFORGE_PACKAGE_INTERNAL_H

#include "flowie_control_release_archive_internal.h"
#include "flowie_rulesforge_worker_internal.h"

#define FLOWIE_RULESFORGE_FLOW_PATH FLOWIE_RELEASE_RULESFORGE_DIRECTORY "flow.json"

typedef struct flowie_rulesforge_package_s flowie_rulesforge_package_t;

typedef struct flowie_rulesforge_package_config_s {
  const char *executable;
  const char *provider;
  const char *staging_root;
  uint32_t timeout_ms;
} flowie_rulesforge_package_config_t;

/* One exclusive owner. Config paths are deployment-controlled absolute paths. The staging
 * root must be private to the server account. The provider is an immutable regular file with
 * its runtime dependencies available to the worker. Preparation verifies the script-only
 * descriptor, stages signed assets and waits for worker readiness. On success *out owns the
 * candidate and the archive and config may be freed. On error *out is NULL. */
int flowie_rulesforge_package_prepare(const flowie_rulesforge_package_config_t *config,
    const flowie_release_archive_t *archive, flowie_rulesforge_package_t **out);

/* The worker borrows package lifetime. Settle its outputs before retiring the package. */
flowie_rulesforge_worker_t *flowie_rulesforge_package_worker(flowie_rulesforge_package_t *package);
int flowie_rulesforge_package_close(flowie_rulesforge_package_t **owner);

#endif
