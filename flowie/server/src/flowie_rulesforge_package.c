#include "flowie_rulesforge_package_internal.h"

#include <core/error.h>
#include <core/uuid.h>
#include <file/fs.h>
#include <json_parser.h>

#include <stdlib.h>
#include <string.h>

enum {
  FLOWIE_RULESFORGE_DESCRIPTOR_MAX = 64 * 1024,
  FLOWIE_RULESFORGE_PROVIDER_MAX = 32 * 1024 * 1024,
  FLOWIE_RULESFORGE_STAGE_PATH_MAX = 4096,
  FLOWIE_RULESFORGE_RELATIVE_PATH_SIZE = SALTS_UUID_STRING_SIZE + FLOWIE_RELEASE_MAX_PATH + 1
};

struct flowie_rulesforge_package_s {
  fs_root_t *root;
  char directory[SALTS_UUID_STRING_SIZE];
  char files[FLOWIE_RELEASE_MAX_FILES + 1u][FLOWIE_RELEASE_MAX_PATH + 1u];
  size_t file_count;
  flowie_rulesforge_worker_t *worker;
};

typedef struct flowie_rulesforge_descriptor_s {
  size_t depth;
  uint32_t keys[4]; /* Bundle, bindings array, binding object, parameter array. */
} flowie_rulesforge_descriptor_t;

static int descriptor_enter(void *context) {
  flowie_rulesforge_descriptor_t *descriptor = context;
  if (descriptor->depth == sizeof(descriptor->keys) / sizeof(descriptor->keys[0])) return -1;
  descriptor->keys[descriptor->depth++] = 0;
  return 0;
}

static int descriptor_leave(void *context) {
  flowie_rulesforge_descriptor_t *descriptor = context;
  --descriptor->depth;
  return 0;
}

static int descriptor_key(void *context, const char *key, size_t size) {
  static const char *const keys[] = {
      "provider", "rules", "signatures", "mode", "modules", "bindings", "limits",
      "name", "path", "module", "function", "parameters", "result", "source_bytes", "value_bytes",
      "value_nodes", "value_depth", "steps", "loops", "recursion", "retained_bytes", "stack_bytes",
      "max_sessions"};
  enum { BUNDLE_KEY_COUNT = 7 };
  flowie_rulesforge_descriptor_t *descriptor = context;
  /* RulesForge resolves native_modules at Program construction, before enforcing
   * the Continuous Session contract. Admit only script-only keys and reject duplicates
   * so Flowie and the worker cannot interpret different field values. */
  const size_t first = descriptor->depth == 1u ? 0u : BUNDLE_KEY_COUNT;
  const size_t end = descriptor->depth == 1u ? BUNDLE_KEY_COUNT : sizeof(keys) / sizeof(keys[0]);
  uint32_t *seen = &descriptor->keys[descriptor->depth - 1u];
  for (size_t i = first; i < end; ++i) {
    if (strlen(keys[i]) != size || memcmp(key, keys[i], size) != 0) continue;
    const uint32_t bit = UINT32_C(1) << i;
    if (*seen & bit) return -1;
    *seen |= bit;
    return 0;
  }
  return -1;
}

static int file_suffix(const char *name, size_t size, const char *suffix) {
  const size_t length = strlen(suffix);
  return size > length && memcmp(name + size - length, suffix, length) == 0;
}

static int descriptor_reference(const json_value_t *value, const char *suffix) {
  if (!value || json_type(value) != JSON_STRING) return 0;
  const char *name = json_string(value);
  const size_t size = json_string_len(value);
  return flowie_control_release_path_valid(name, size) && !memchr(name, '/', size) &&
      file_suffix(name, size, suffix);
}

static int descriptor_check(const void *bytes, size_t size, const char *provider) {
  static const json_sax_handler_raw_t callbacks = {.on_object_start = descriptor_enter,
      .on_object_end = descriptor_leave, .on_array_start = descriptor_enter,
      .on_array_end = descriptor_leave, .on_object_key = descriptor_key};
  flowie_rulesforge_descriptor_t descriptor = {0};
  if (!size || size > FLOWIE_RULESFORGE_DESCRIPTOR_MAX || memchr(bytes, '\0', size) ||
      json_parse_sax_raw(bytes, size, &callbacks, &descriptor) != 0)
    return SALTS_EPROTO;
  json_value_t *document = json_parse(bytes, size);
  if (!document) return SALTS_EPROTO;
  const json_value_t *native = json_object_get(document, "provider");
  const json_value_t *mode = json_object_get(document, "mode");
  const json_value_t *modules = json_object_get(document, "modules");
  const size_t provider_size = strlen(provider);
  static const char interpreter[] = "interpreter";
  int valid = json_type(document) == JSON_OBJECT && native && json_type(native) == JSON_STRING &&
      json_string_len(native) == provider_size &&
      memcmp(json_string(native), provider, provider_size) == 0 &&
      mode && json_type(mode) == JSON_STRING && json_string_len(mode) == sizeof(interpreter) - 1u &&
      memcmp(json_string(mode), interpreter, sizeof(interpreter) - 1u) == 0 &&
      descriptor_reference(json_object_get(document, "rules"), ".rfl") &&
      descriptor_reference(json_object_get(document, "signatures"), ".schema") &&
      modules && json_type(modules) == JSON_ARRAY;
  for (size_t i = 0; valid && i < json_array_size(modules); ++i) {
    const json_value_t *module = json_array_get(modules, i);
    valid = descriptor_reference(json_object_get(module, "path"), ".toad");
  }
  json_free(document);
  return valid ? SALTS_OK : SALTS_EPROTO;
}

static int package_assets_check(const flowie_release_manifest_t *manifest) {
  for (size_t i = 0; i < manifest->file_count; ++i) {
    const char *path = manifest->files[i].path;
    if (strcmp(path, FLOWIE_RULESFORGE_FLOW_PATH) == 0) continue;
    if (strncmp(path, FLOWIE_RELEASE_RULESFORGE_DIRECTORY, sizeof(FLOWIE_RELEASE_RULESFORGE_DIRECTORY) - 1u))
      continue;
    const char *name = path + sizeof(FLOWIE_RELEASE_RULESFORGE_DIRECTORY) - 1u;
    const size_t size = strlen(name);
    if (strchr(name, '/') || (strcmp(name, FLOWIE_RELEASE_RULESFORGE_PROGRAM) != 0 &&
        !file_suffix(name, size, ".rfl") && !file_suffix(name, size, ".schema") &&
        !file_suffix(name, size, ".toad")))
      return SALTS_EPROTO;
  }
  return SALTS_OK;
}

static int stage_file_open(flowie_rulesforge_package_t *package, const char *name,
    fs_root_file_t **out) {
  char path[FLOWIE_RULESFORGE_RELATIVE_PATH_SIZE];
  int rc = fs_path_join(path, sizeof(path), package->directory, name);
  if (rc != SALTS_OK) return rc;
  rc = fs_root_file_open(package->root, path,
      SALTS_FS_O_WRONLY | SALTS_FS_O_CREAT | SALTS_FS_ROOT_O_EXCL, 0600, out);
  if (rc == SALTS_OK) strcpy(package->files[package->file_count++], name);
  return rc;
}

static int stage_file_write(fs_root_file_t *file, const char *data, size_t size) {
  while (size) {
    const int written = fs_root_file_write(file, data, size);
    if (written < 0) return written;
    if (!written) return SALTS_EIO;
    size -= (size_t)written;
    data += written;
  }
  return SALTS_OK;
}

static int stage_asset(flowie_rulesforge_package_t *package, const char *name,
    const void *bytes, size_t size) {
  fs_root_file_t *file = NULL;
  int rc = stage_file_open(package, name, &file);
  if (rc != SALTS_OK) return rc;
  rc = stage_file_write(file, bytes, size);
  const int closed = fs_root_file_close(file);
  return rc == SALTS_OK ? closed : rc;
}

static int stage_provider(flowie_rulesforge_package_t *package, const char *name, const char *path) {
  char directory[FLOWIE_RULESFORGE_STAGE_PATH_MAX];
  int rc = fs_path_dirname(path, directory, sizeof(directory));
  if (rc != SALTS_OK) return rc;
#ifdef _WIN32
  /* dirname strips the separator from a drive root, leaving a drive-relative path. */
  if (directory[1] == ':' && directory[2] == '\0') {
    directory[2] = path[2];
    directory[3] = '\0';
  }
#endif
  fs_root_t *root = NULL;
  rc = fs_root_open(directory, &root);
  if (rc != SALTS_OK) return rc;
  fs_root_file_t *source = NULL;
  fs_root_file_t *file = NULL;
  fs_stat_t stat = {0};
  rc = fs_root_file_open(root, name, SALTS_FS_O_RDONLY, 0, &source);
  if (rc == SALTS_OK) rc = fs_root_file_stat(source, &stat);
  if (rc == SALTS_OK && (!stat.size || stat.size > FLOWIE_RULESFORGE_PROVIDER_MAX)) rc = SALTS_EINVAL;
  if (rc == SALTS_OK) rc = stage_file_open(package, name, &file);
  char block[4096];
  size_t remaining = (size_t)stat.size;
  while (rc == SALTS_OK && remaining) {
    const int read = fs_root_file_read(source, block,
        remaining < sizeof(block) ? remaining : sizeof(block));
    if (read <= 0) {
      rc = read < 0 ? read : SALTS_EIO;
      break;
    }
    rc = stage_file_write(file, block, (size_t)read);
    remaining -= (size_t)read;
  }
  if (rc == SALTS_OK) {
    const int read = fs_root_file_read(source, block, 1u);
    if (read != 0) rc = read < 0 ? read : SALTS_EIO;
  }
  const int source_closed = source ? fs_root_file_close(source) : SALTS_OK;
  const int target_closed = file ? fs_root_file_close(file) : SALTS_OK;
  const int root_closed = fs_root_close(root);
  if (rc == SALTS_OK) rc = source_closed == SALTS_OK ? target_closed : source_closed;
  if (rc == SALTS_OK) rc = root_closed;
  return rc;
}

int flowie_rulesforge_package_prepare(const flowie_rulesforge_package_config_t *config,
    const flowie_release_archive_t *archive, flowie_rulesforge_package_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!archive || !config || !config->timeout_ms || !config->executable || !config->provider ||
      !config->staging_root || !fs_path_is_absolute(config->executable) ||
      !fs_path_is_absolute(config->provider) || !fs_path_is_absolute(config->staging_root))
    return SALTS_EINVAL;

  char provider[FLOWIE_RELEASE_MAX_PATH + 1u];
  int rc = fs_path_basename(config->provider, provider, sizeof(provider));
  if (rc != SALTS_OK) return rc;
  const size_t provider_size = strlen(provider);
#ifdef _WIN32
  const char *suffix = ".dll";
#elif defined(__APPLE__)
  const char *suffix = ".dylib";
#else
  const char *suffix = ".so";
#endif
  if (!flowie_control_release_path_valid(provider, provider_size) ||
      !file_suffix(provider, provider_size, suffix))
    return SALTS_EINVAL;

  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(archive);
  if (manifest->rulesforge_index == manifest->file_count) return SALTS_EPROTO;
  rc = package_assets_check(manifest);
  if (rc != SALTS_OK) return rc;
  const void *bytes = NULL;
  size_t size = 0;
  rc = flowie_control_release_archive_file(archive, manifest->files[manifest->rulesforge_index].path,
      &bytes, &size);
  if (rc != SALTS_OK) return rc;
  rc = descriptor_check(bytes, size, provider);
  if (rc != SALTS_OK) return rc;

  char directory[SALTS_UUID_STRING_SIZE];
  unsigned char id[SALTS_UUID_SIZE];
  rc = uuid_v4_generate(id);
  if (rc == SALTS_OK) rc = uuid_format(id, directory, sizeof(directory));
  if (rc != SALTS_OK) return rc;
  char relative[FLOWIE_RULESFORGE_RELATIVE_PATH_SIZE];
  char descriptor_path[FLOWIE_RULESFORGE_STAGE_PATH_MAX];
  rc = fs_path_join(relative, sizeof(relative), directory, FLOWIE_RELEASE_RULESFORGE_PROGRAM);
  if (rc == SALTS_OK)
    rc = fs_path_join(descriptor_path, sizeof(descriptor_path), config->staging_root, relative);
  if (rc != SALTS_OK) return rc;

  flowie_rulesforge_package_t *package = calloc(1, sizeof(*package));
  if (!package) return SALTS_ENOMEM;
  rc = fs_root_open(config->staging_root, &package->root);
  if (rc == SALTS_OK) rc = fs_root_mkdir(package->root, directory, 0700);
  if (rc == SALTS_OK) {
    strcpy(package->directory, directory);
    rc = stage_provider(package, provider, config->provider);
  }
  for (size_t i = 0; rc == SALTS_OK && i < manifest->file_count; ++i) {
    const char *path = manifest->files[i].path;
    if (strcmp(path, FLOWIE_RULESFORGE_FLOW_PATH) == 0) continue;
    if (strncmp(path, FLOWIE_RELEASE_RULESFORGE_DIRECTORY, sizeof(FLOWIE_RELEASE_RULESFORGE_DIRECTORY) - 1u))
      continue;
    rc = flowie_control_release_archive_file(archive, path, &bytes, &size);
    if (rc == SALTS_OK)
      rc = stage_asset(package, path + sizeof(FLOWIE_RELEASE_RULESFORGE_DIRECTORY) - 1u, bytes, size);
  }
  if (rc == SALTS_OK)
    rc = flowie_rulesforge_worker_open(config->executable, descriptor_path, config->timeout_ms,
        &package->worker);
  if (rc != SALTS_OK) {
    (void)flowie_rulesforge_package_close(&package);
    return rc;
  }
  *out = package;
  return SALTS_OK;
}

flowie_rulesforge_worker_t *flowie_rulesforge_package_worker(flowie_rulesforge_package_t *package) {
  return package ? package->worker : NULL;
}

int flowie_rulesforge_package_close(flowie_rulesforge_package_t **owner) {
  if (!owner) return SALTS_EINVAL;
  flowie_rulesforge_package_t *package = *owner;
  if (!package) return SALTS_OK;
  *owner = NULL;
  int rc = flowie_rulesforge_worker_close(&package->worker);
  for (size_t i = 0; i < package->file_count; ++i) {
    char path[FLOWIE_RULESFORGE_RELATIVE_PATH_SIZE];
    int removed = fs_path_join(path, sizeof(path), package->directory, package->files[i]);
    if (removed == SALTS_OK) removed = fs_root_unlink(package->root, path);
    if (rc == SALTS_OK) rc = removed;
  }
  if (package->directory[0]) {
    const int removed = fs_root_rmdir(package->root, package->directory);
    if (rc == SALTS_OK) rc = removed;
  }
  if (package->root) {
    const int closed = fs_root_close(package->root);
    if (rc == SALTS_OK) rc = closed;
  }
  free(package);
  return rc;
}
