#include "flowie_control_release_archive_internal.h"

#include <core/error.h>
#include <core/zstd.h>
#include <crypto/sha256.h>
#include <crypto/sm3.h>

/* The pinned vcpkg static library supplies the frame admission checks missing from Core. */
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

#include <stdlib.h>
#include <string.h>

enum {
  TAR_BLOCK = 512,
  TAR_NAME_SIZE = 100,
  TAR_SIZE = 124,
  TAR_SIZE_WIDTH = 12,
  TAR_CHECKSUM = 148,
  TAR_CHECKSUM_WIDTH = 8,
  TAR_TYPE = 156,
  TAR_LINK = 157,
  TAR_MAGIC = 257,
  TAR_PREFIX = 345,
  TAR_PREFIX_SIZE = 155
};

struct flowie_release_archive_s {
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  uint8_t *data;
  size_t size;
  flowie_release_manifest_t *manifest;
  size_t offsets[FLOWIE_RELEASE_MAX_FILES];
  flowie_release_manifest_file_t files[FLOWIE_RELEASE_MAX_FILES];
};

static int flowie_release_all_zero(const uint8_t *bytes, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    if (bytes[i] != 0) return 0;
  }
  return 1;
}

static int flowie_release_tar_octal(const uint8_t *field, size_t width, size_t *out) {
  size_t end = 0, value = 0;
  while (end < width && field[end] == ' ') ++end;
  const size_t start = end;
  while (end < width && field[end] >= '0' && field[end] <= '7') {
    const size_t digit = (size_t)(field[end++] - '0');
    if (value > (SIZE_MAX - digit) / 8u) return 0;
    value = value * 8u + digit;
  }
  if (end == start || end == width) return 0;
  while (end < width && (field[end] == 0 || field[end] == ' ')) ++end;
  if (end != width) return 0;
  *out = value;
  return 1;
}

static int flowie_release_tar_field(const uint8_t *field, size_t width, size_t *length) {
  const uint8_t *end = memchr(field, 0, width);
  *length = end ? (size_t)(end - field) : width;
  return flowie_release_all_zero(field + *length, width - *length);
}

static int flowie_release_tar_header(const uint8_t *header, char *path, size_t *size) {
  static const uint8_t ustar[] = {'u', 's', 't', 'a', 'r', 0, '0', '0'};
  if (memcmp(header + TAR_MAGIC, ustar, sizeof(ustar)) != 0 ||
      (header[TAR_TYPE] != '0' && header[TAR_TYPE] != 0) ||
      !flowie_release_all_zero(header + TAR_LINK, TAR_NAME_SIZE))
    return SALTS_EPROTO;

  size_t checksum = 0, computed = 0;
  if (!flowie_release_tar_octal(header + TAR_CHECKSUM, TAR_CHECKSUM_WIDTH, &checksum) ||
      !flowie_release_tar_octal(header + TAR_SIZE, TAR_SIZE_WIDTH, size))
    return SALTS_EPROTO;
  for (size_t i = 0; i < TAR_BLOCK; ++i) {
    computed += i >= TAR_CHECKSUM && i < TAR_CHECKSUM + TAR_CHECKSUM_WIDTH ? ' ' : header[i];
  }
  if (checksum != computed) return SALTS_EPROTO;
  if (*size > FLOWIE_RELEASE_MAX_ENTRY_BYTES) return SALTS_ENOSPC;

  size_t name = 0, prefix = 0;
  if (!flowie_release_tar_field(header, TAR_NAME_SIZE, &name) || !name ||
      !flowie_release_tar_field(header + TAR_PREFIX, TAR_PREFIX_SIZE, &prefix) ||
      name + prefix + (prefix != 0u) > FLOWIE_RELEASE_MAX_PATH)
    return SALTS_EPROTO;
  size_t path_size = 0;
  if (prefix) {
    memcpy(path, header + TAR_PREFIX, prefix);
    path[prefix] = '/';
    path_size = prefix + 1u;
  }
  memcpy(path + path_size, header, name);
  path_size += name;
  if (!flowie_control_release_path_valid(path, path_size)) return SALTS_EPROTO;
  path[path_size] = '\0';
  return SALTS_OK;
}

static int flowie_release_tar_read(flowie_release_archive_t *archive, size_t data_size,
    const salts_crypto_sm2_cms_verify_config *signer, const char *domain, const char *environment) {
  const uint8_t *manifest = NULL, *signature = NULL;
  size_t manifest_size = 0, signature_size = 0, count = 0, position = 0;
  while (position <= data_size - TAR_BLOCK) {
    const uint8_t *header = archive->data + position;
    if (flowie_release_all_zero(header, TAR_BLOCK)) break;
    char path[FLOWIE_RELEASE_MAX_PATH + 1];
    size_t size = 0;
    int rc = flowie_release_tar_header(header, path, &size);
    if (rc != SALTS_OK) return rc;
    position += TAR_BLOCK;
    const size_t padded = (size + TAR_BLOCK - 1u) / TAR_BLOCK * TAR_BLOCK;
    if (padded > data_size - position ||
        !flowie_release_all_zero(archive->data + position + size, padded - size))
      return SALTS_EPROTO;

    if (strcmp(path, "manifest.json") == 0) {
      if (manifest || size == 0u) return SALTS_EPROTO;
      if (size > SALTS_CRYPTO_SM2_CMS_MAX_CONTENT) return SALTS_ENOSPC;
      manifest = archive->data + position;
      manifest_size = size;
    } else if (strcmp(path, "manifest.p7s") == 0) {
      if (signature || size == 0u) return SALTS_EPROTO;
      if (size > SALTS_CRYPTO_SM2_CMS_MAX_DER) return SALTS_ENOSPC;
      signature = archive->data + position;
      signature_size = size;
    } else {
      if (count == FLOWIE_RELEASE_MAX_FILES) return SALTS_ENOSPC;
      strcpy(archive->files[count].path, path);
      archive->files[count].size = size;
      archive->offsets[count++] = position;
    }
    position += padded;
  }
  if (!manifest || !signature || data_size - position < 2u * TAR_BLOCK ||
      !flowie_release_all_zero(archive->data + position, data_size - position))
    return SALTS_EPROTO;
  int rc = flowie_control_release_manifest_verify(signer, domain, environment, manifest,
      manifest_size, signature, signature_size, &archive->manifest);
  if (rc != SALTS_OK) return rc;
  if (count != archive->manifest->file_count) return SALTS_EPROTO;
  for (size_t i = 0; i < count; ++i) {
    if (salts_crypto_sm3(archive->data + archive->offsets[i], archive->files[i].size,
                          archive->files[i].sm3) != SALTS_CRYPTO_OK)
      return SALTS_EIO;
  }
  return flowie_control_release_manifest_check_files(archive->manifest, archive->files, count);
}

int flowie_control_release_archive_verify(const salts_crypto_sm2_cms_verify_config *signer,
    const char *domain, const char *environment, const void *bytes, size_t size,
    flowie_release_archive_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!signer || !domain || !environment || !bytes || !size) return SALTS_EINVAL;
  if (size > FLOWIE_RELEASE_MAX_COMPRESSED_BYTES) return SALTS_ENOSPC;
  ZSTD_FrameHeader frame;
  if (ZSTD_getFrameHeader(&frame, bytes, size) != 0 || frame.frameType != ZSTD_frame ||
      frame.dictID != 0 || frame.frameContentSize == ZSTD_CONTENTSIZE_UNKNOWN)
    return SALTS_EPROTO;
  if (frame.frameContentSize > FLOWIE_RELEASE_MAX_EXPANDED_BYTES ||
      frame.windowSize > FLOWIE_RELEASE_MAX_EXPANDED_BYTES)
    return SALTS_ENOSPC;
  const size_t expected_size = (size_t)frame.frameContentSize;
  if (expected_size < 2u * TAR_BLOCK || expected_size % TAR_BLOCK != 0 ||
      ZSTD_findFrameCompressedSize(bytes, size) != size)
    return SALTS_EPROTO;

  flowie_release_archive_t *archive = calloc(1, sizeof(*archive));
  if (!archive) return SALTS_ENOMEM;
  archive->data = malloc(expected_size);
  int rc = SALTS_ENOMEM;
  if (!archive->data) goto failure;
  size_t actual_size = 0;
  rc = zstd_decompress(archive->data, expected_size, &actual_size, bytes, size);
  if (rc != SALTS_OK) goto failure;
  rc = SALTS_EPROTO;
  if (expected_size != actual_size) goto failure;
  rc = flowie_release_tar_read(archive, actual_size, signer, domain, environment);
  if (rc != SALTS_OK) goto failure;
  if (salts_crypto_sha256(bytes, size, archive->digest) != SALTS_CRYPTO_OK) {
    rc = SALTS_EIO;
    goto failure;
  }
  archive->size = actual_size;
  *out = archive;
  return SALTS_OK;
failure:
  flowie_control_release_archive_destroy(archive);
  return rc;
}

void flowie_control_release_archive_destroy(flowie_release_archive_t *archive) {
  if (!archive) return;
  free(archive->manifest);
  free(archive->data);
  free(archive);
}

const uint8_t *flowie_control_release_archive_digest(const flowie_release_archive_t *archive) {
  return archive->digest;
}

const flowie_release_manifest_t *flowie_control_release_archive_manifest(
    const flowie_release_archive_t *archive) {
  return archive->manifest;
}

const void *flowie_control_release_archive_tar(const flowie_release_archive_t *archive, size_t *size) {
  *size = archive->size;
  return archive->data;
}

int flowie_control_release_archive_file(const flowie_release_archive_t *archive, const char *path,
    const void **data, size_t *size) {
  if (!archive || !path || !data || !size) return SALTS_EINVAL;
  for (size_t i = 0; i < archive->manifest->file_count; ++i) {
    if (strcmp(archive->files[i].path, path) != 0) continue;
    *data = archive->data + archive->offsets[i];
    *size = archive->files[i].size;
    return SALTS_OK;
  }
  return SALTS_ENOENT;
}
