#ifndef FLOWIE_CONTROL_RELEASE_ARCHIVE_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_ARCHIVE_INTERNAL_H

#include "flowie_control_release_manifest_internal.h"

enum { FLOWIE_RELEASE_MAX_COMPRESSED_BYTES = 8 * 1024 * 1024 };

typedef struct flowie_release_archive_s flowie_release_archive_t;

/* Verify one size-declared Zstd frame containing a USTAR release. Config and compressed bytes
 * are immutable borrows through return. On success the caller owns an archive with verified
 * payloads. The release digest identifies the exact compressed input, which the caller must keep
 * unchanged if it persists those bytes. On failure *out is NULL. */
int flowie_control_release_archive_verify(const salts_crypto_sm2_cms_verify_config *signer,
    const char *domain, const char *environment, const void *bytes, size_t size,
    flowie_release_archive_t **out);

void flowie_control_release_archive_destroy(flowie_release_archive_t *archive);

/* Views borrow immutable archive storage and expire at destruction. */
const uint8_t *flowie_control_release_archive_digest(const flowie_release_archive_t *archive);
const flowie_release_manifest_t *flowie_control_release_archive_manifest(
    const flowie_release_archive_t *archive);
/* Borrow all verified USTAR bytes until the archive is destroyed. */
const void *flowie_control_release_archive_tar(const flowie_release_archive_t *archive, size_t *size);
int flowie_control_release_archive_file(const flowie_release_archive_t *archive, const char *path,
    const void **data, size_t *size);

#endif
