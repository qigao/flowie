#include "flowie_control_release_manifest_internal.h"

#include <core/error.h>
#include <crypto/sm3.h>
#include <json_parser.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

typedef struct flowie_release_keys_s {
  unsigned int seen[3]; /* Root object, files array and file object at maximum depth. */
  size_t depth;
} flowie_release_keys_t;

static int flowie_release_open_container(void *context) {
  flowie_release_keys_t *keys = context;
  if (keys->depth == sizeof(keys->seen) / sizeof(keys->seen[0])) return -1;
  keys->seen[keys->depth++] = 0;
  return 0;
}

static int flowie_release_close_container(void *context) {
  flowie_release_keys_t *keys = context;
  --keys->depth;
  return 0;
}

static int flowie_release_key(void *context, const char *key, size_t size) {
  static const char *const names[] = {"domain", "environment", "base_release", "entrypoints",
                                      "files", "acl", "routes", "praktor", "path", "size", "sm3",
                                      "name", "rulesforge"};
  flowie_release_keys_t *keys = context;
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
    if (strlen(names[i]) != size || memcmp(key, names[i], size) != 0) continue;
    unsigned int *seen = &keys->seen[keys->depth - 1u];
    const unsigned int bit = 1u << i;
    if ((*seen & bit) != 0u) return -1;
    *seen |= bit;
    return 0;
  }
  return -1;
}

static int flowie_release_text_equal(const json_value_t *value, const char *text) {
  const size_t size = strlen(text);
  return value && json_type(value) == JSON_STRING && json_string_len(value) == size &&
         memcmp(json_string(value), text, size) == 0;
}

static int flowie_release_hex_digit(char digit) {
  if (digit >= '0' && digit <= '9') return digit - '0';
  if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
  return -1;
}

static int flowie_release_digest(const json_value_t *value, uint8_t *out) {
  if (!value || json_type(value) != JSON_STRING ||
      json_string_len(value) != FLOWIE_RELEASE_DIGEST_BYTES * 2u)
    return SALTS_EPROTO;
  const char *text = json_string(value);
  for (size_t i = 0; i < FLOWIE_RELEASE_DIGEST_BYTES; ++i) {
    const int high = flowie_release_hex_digit(text[i * 2u]);
    const int low = flowie_release_hex_digit(text[i * 2u + 1u]);
    if (high < 0 || low < 0) return SALTS_EPROTO;
    out[i] = (uint8_t)(high * 16 + low);
  }
  return SALTS_OK;
}

static int flowie_release_size(const json_value_t *value, size_t *out) {
  size_t size = 0, parsed = 0;
  const char *text = value ? json_number_text(value, &size) : NULL;
  if (!text || !size) return SALTS_EPROTO;
  for (size_t i = 0; i < size; ++i) {
    if (text[i] < '0' || text[i] > '9') return SALTS_EPROTO;
    const size_t digit = (size_t)(text[i] - '0');
    if (parsed > (FLOWIE_RELEASE_MAX_ENTRY_BYTES - digit) / 10u) return SALTS_ENOSPC;
    parsed = parsed * 10u + digit;
  }
  *out = parsed;
  return SALTS_OK;
}

static int flowie_release_device_name(const char *part, size_t size) {
  const char *dot = memchr(part, '.', size);
  const size_t stem = dot ? (size_t)(dot - part) : size;
  if (stem == 3u) {
    return memcmp(part, "aux", 3u) == 0 || memcmp(part, "con", 3u) == 0 ||
           memcmp(part, "nul", 3u) == 0 || memcmp(part, "prn", 3u) == 0;
  }
  return stem == 4u && part[3] >= '0' && part[3] <= '9' &&
         (memcmp(part, "com", 3u) == 0 || memcmp(part, "lpt", 3u) == 0);
}

int flowie_control_release_path_valid(const char *path, size_t size) {
  if (!path || !size || size > FLOWIE_RELEASE_MAX_PATH) return 0;
  size_t start = 0;
  for (size_t i = 0; i <= size; ++i) {
    if (i == size || path[i] == '/') {
      if (i == start || path[i - 1u] == '.' || flowie_release_device_name(path + start, i - start))
        return 0;
      start = i + 1u;
      continue;
    }
    const char c = path[i];
    if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '.' && c != '_' && c != '-')
      return 0;
  }
  return 1;
}

static int flowie_release_path(const json_value_t *value, char *out) {
  if (!value || json_type(value) != JSON_STRING) return SALTS_EPROTO;
  const size_t size = json_string_len(value);
  const char *path = json_string(value);
  if (!flowie_control_release_path_valid(path, size)) return SALTS_EPROTO;
  memcpy(out, path, size);
  out[size] = '\0';
  return SALTS_OK;
}

static int flowie_release_paths_conflict(const char *left, const char *right) {
  const size_t left_size = strlen(left), right_size = strlen(right);
  const size_t shared = left_size < right_size ? left_size : right_size;
  return memcmp(left, right, shared) == 0 &&
         (left_size == right_size || left[shared] == '/' || right[shared] == '/');
}

static int flowie_release_name(const json_value_t *value, char *out) {
  if (!value) return SALTS_OK;
  if (json_type(value) != JSON_STRING) return SALTS_EPROTO;
  const size_t size = json_string_len(value);
  if (!size || size > FLOWIE_RELEASE_MAX_NAME) return SALTS_EPROTO;
  const char *name = json_string(value);
  for (size_t i = 0; i < size; ++i) {
    const char c = name[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
    if (!i || (c != '.' && c != '_' && c != '-')) return SALTS_EPROTO;
  }
  memcpy(out, name, size);
  out[size] = '\0';
  return SALTS_OK;
}

static int flowie_release_manifest_parse(const json_value_t *document, const char *domain,
    const char *environment, size_t metadata_bytes, flowie_release_manifest_t **out) {
  if (json_type(document) != JSON_OBJECT) return SALTS_EPROTO;
  const json_value_t *name = json_object_get(document, "name");
  if (json_object_size(document) != 5u + (name != NULL)) return SALTS_EPROTO;
  const json_value_t *domain_value = json_object_get(document, "domain");
  const json_value_t *environment_value = json_object_get(document, "environment");
  const json_value_t *base = json_object_get(document, "base_release");
  const json_value_t *entrypoints = json_object_get(document, "entrypoints");
  const json_value_t *files = json_object_get(document, "files");
  const json_value_t *acl = json_object_get(entrypoints, "acl");
  const json_value_t *routes = json_object_get(entrypoints, "routes");
  const json_value_t *praktor = json_object_get(entrypoints, "praktor");
  const json_value_t *rulesforge = json_object_get(entrypoints, "rulesforge");
  if (!domain_value || json_type(domain_value) != JSON_STRING || !environment_value ||
      json_type(environment_value) != JSON_STRING || !base || !files ||
      json_type(files) != JSON_ARRAY || !entrypoints || json_type(entrypoints) != JSON_OBJECT ||
      !acl || json_object_size(entrypoints) !=
          1u + (routes != NULL) + (praktor != NULL) + (rulesforge != NULL) ||
      (rulesforge && !flowie_release_text_equal(rulesforge, FLOWIE_RELEASE_RULESFORGE_PATH)))
    return SALTS_EPROTO;
  if (!flowie_release_text_equal(domain_value, domain) ||
      !flowie_release_text_equal(environment_value, environment))
    return SALTS_EPERM;

  const size_t count = json_array_size(files);
  if (count < 1u + (routes != NULL) + (praktor != NULL) + (rulesforge != NULL)) return SALTS_EPROTO;
  if (count > FLOWIE_RELEASE_MAX_FILES) return SALTS_ENOSPC;
  flowie_release_manifest_t *result = calloc(1u, sizeof(*result) + count * sizeof(result->files[0]));
  if (!result) return SALTS_ENOMEM;
  result->file_count = count;
  result->acl_index = result->routes_index = result->praktor_index = count;
  result->rulesforge_index = count;
  int rc = flowie_release_name(name, result->name);
  if (rc != SALTS_OK) goto fail;
  result->has_base_release = json_type(base) != JSON_NULL;
  if (result->has_base_release) {
    rc = flowie_release_digest(base, result->base_release);
    if (rc != SALTS_OK) goto fail;
  }

  size_t total = metadata_bytes;
  for (size_t i = 0; i < count; ++i) {
    const json_value_t *entry = json_array_get(files, i);
    flowie_release_manifest_file_t *file = &result->files[i];
    rc = SALTS_EPROTO;
    if (json_type(entry) != JSON_OBJECT || json_object_size(entry) != 3u ||
        flowie_release_path(json_object_get(entry, "path"), file->path) != SALTS_OK ||
        flowie_release_digest(json_object_get(entry, "sm3"), file->sm3) != SALTS_OK ||
        flowie_release_paths_conflict(file->path, "manifest.json") ||
        flowie_release_paths_conflict(file->path, "manifest.p7s"))
      goto fail;
    for (size_t j = 0; j < i; ++j) {
      if (flowie_release_paths_conflict(file->path, result->files[j].path)) goto fail;
    }
    rc = flowie_release_size(json_object_get(entry, "size"), &file->size);
    if (rc != SALTS_OK) goto fail;
    if (file->size > FLOWIE_RELEASE_MAX_EXPANDED_BYTES - total) {
      rc = SALTS_ENOSPC;
      goto fail;
    }
    total += file->size;
    if (flowie_release_text_equal(acl, file->path)) result->acl_index = i;
    if (flowie_release_text_equal(routes, file->path)) result->routes_index = i;
    if (strcmp(file->path, FLOWIE_RELEASE_RULESFORGE_PATH) == 0) {
      rc = SALTS_EPROTO;
      if (!rulesforge || !file->size) goto fail;
      result->rulesforge_index = i;
    }
    if (flowie_release_text_equal(praktor, file->path)) {
      rc = SALTS_EPROTO;
      if (!file->size) goto fail;
      rc = SALTS_ENOSPC;
      if (file->size > FLOWIE_RELEASE_MAX_PRAKTOR_BYTES) goto fail;
      result->praktor_index = i;
    }
  }
  if (result->acl_index == count ||
      (routes && (result->routes_index == count || result->acl_index == result->routes_index)) ||
      (rulesforge && (result->rulesforge_index == count || result->rulesforge_index == result->acl_index ||
          result->rulesforge_index == result->routes_index || result->rulesforge_index == result->praktor_index)) ||
      (praktor && (result->praktor_index == count || result->praktor_index == result->acl_index ||
                   result->praktor_index == result->routes_index))) {
    rc = SALTS_EPROTO;
    goto fail;
  }
  *out = result;
  return SALTS_OK;
fail:
  free(result);
  return rc;
}

int flowie_control_release_manifest_check_files(const flowie_release_manifest_t *manifest,
    const flowie_release_manifest_file_t *files, size_t count) {
  if (!manifest || (!files && count)) return SALTS_EINVAL;
  if (count > FLOWIE_RELEASE_MAX_FILES) return SALTS_ENOSPC;
  if (count != manifest->file_count) return SALTS_EPROTO;
  bool seen[FLOWIE_RELEASE_MAX_FILES] = {0};
  for (size_t i = 0; i < count; ++i) {
    if (!memchr(files[i].path, '\0', sizeof(files[i].path))) return SALTS_EPROTO;
    size_t match = 0;
    while (match < count && strcmp(manifest->files[match].path, files[i].path) != 0) ++match;
    if (match == count || seen[match] || manifest->files[match].size != files[i].size ||
        memcmp(manifest->files[match].sm3, files[i].sm3, sizeof(files[i].sm3)) != 0)
      return SALTS_EPROTO;
    seen[match] = true;
  }
  return SALTS_OK;
}

int flowie_control_release_manifest_verify(const salts_crypto_sm2_cms_verify_config *signer,
    const char *domain, const char *environment, const void *manifest, size_t manifest_size,
    const void *signature, size_t signature_size, flowie_release_manifest_t **out) {
  static const json_sax_handler_raw_t keys_handler = {
      .on_object_start = flowie_release_open_container, .on_object_key = flowie_release_key,
      .on_object_end = flowie_release_close_container, .on_array_start = flowie_release_open_container,
      .on_array_end = flowie_release_close_container};
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!signer || !signer->trust || signer->format != SALTS_CRYPTO_SM2_CMS_RFC5652 ||
      !signer->detached || signer->trust->purpose != SALTS_CRYPTO_SM2_TRUST_CODE_SIGNING ||
      !domain || !domain[0] || !environment || !environment[0] || !manifest || !manifest_size ||
      !signature || !signature_size)
    return SALTS_EINVAL;
  if (manifest_size > SALTS_CRYPTO_SM2_CMS_MAX_CONTENT || signature_size > SALTS_CRYPTO_SM2_CMS_MAX_DER)
    return SALTS_ENOSPC;

  char *verified = malloc(SALTS_CRYPTO_SM2_CMS_MAX_CONTENT);
  if (!verified) return SALTS_ENOMEM;
  size_t size = 0;
  const int verified_status = salts_crypto_sm2_cms_verify(signer, signature, signature_size,
      manifest, manifest_size, verified, SALTS_CRYPTO_SM2_CMS_MAX_CONTENT, &size);
  int rc = SALTS_EPROTO;
  json_value_t *document = NULL;
  flowie_release_manifest_t *result = NULL;
  if (verified_status != SALTS_CRYPTO_OK) {
    rc = verified_status == SALTS_CRYPTO_EVERIFY ? SALTS_EPERM : SALTS_EINVAL;
    goto done;
  }
  flowie_release_keys_t keys = {0};
  if (size != manifest_size || memchr(verified, '\0', size) ||
      json_parse_sax_raw(verified, size, &keys_handler, &keys) != 0)
    goto done;
  document = json_parse(verified, size);
  if (!document) goto done;
  rc = flowie_release_manifest_parse(document, domain, environment, size + signature_size, &result);
  if (rc == SALTS_OK) {
    if (salts_crypto_sm3(verified, size, result->sm3) == SALTS_CRYPTO_OK) {
      *out = result;
      result = NULL;
    } else {
      rc = SALTS_EIO;
    }
  }
done:
  if (document) json_free(document);
  free(result);
  free(verified);
  return rc;
}
