#ifndef FLOWIE_CONTROL_RELEASE_MANIFEST_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_MANIFEST_INTERNAL_H

#include <crypto/sm2_cms.h>

#include <stddef.h>
#include <stdint.h>

#define FLOWIE_RELEASE_RULESFORGE_DIRECTORY "rules/"
#define FLOWIE_RELEASE_RULESFORGE_PROGRAM "program.json"
#define FLOWIE_RELEASE_RULESFORGE_PATH FLOWIE_RELEASE_RULESFORGE_DIRECTORY FLOWIE_RELEASE_RULESFORGE_PROGRAM

enum {
  FLOWIE_RELEASE_MAX_ARCHIVE_ENTRIES = 256,
  FLOWIE_RELEASE_MAX_FILES = FLOWIE_RELEASE_MAX_ARCHIVE_ENTRIES - 2,
  FLOWIE_RELEASE_MAX_NAME = 64,
  FLOWIE_RELEASE_MAX_PATH = 240,
  FLOWIE_RELEASE_MAX_ENTRY_BYTES = 8 * 1024 * 1024,
  FLOWIE_RELEASE_MAX_EXPANDED_BYTES = 32 * 1024 * 1024,
  FLOWIE_RELEASE_MAX_PRAKTOR_BYTES = 64 * 1024,
  FLOWIE_RELEASE_DIGEST_BYTES = 32
};

typedef struct flowie_release_manifest_file_s {
  char path[FLOWIE_RELEASE_MAX_PATH + 1];
  size_t size;
  uint8_t sm3[FLOWIE_RELEASE_DIGEST_BYTES];
} flowie_release_manifest_file_t;

typedef struct flowie_release_manifest_s {
  uint8_t sm3[FLOWIE_RELEASE_DIGEST_BYTES];
  uint8_t base_release[FLOWIE_RELEASE_DIGEST_BYTES];
  int has_base_release;
  char name[FLOWIE_RELEASE_MAX_NAME + 1]; /* Empty for legacy signed manifests. */
  size_t acl_index;
  size_t routes_index; /* Legacy routes entry, or file_count when absent. */
  size_t praktor_index; /* Signed instruction text, or file_count when absent. */
  size_t rulesforge_index; /* Signed Program descriptor, or file_count when absent. */
  size_t file_count;
  flowie_release_manifest_file_t files[];
} flowie_release_manifest_t;

/* Validate a complete archive path, including any embedded NUL in the input bytes. */
int flowie_control_release_path_valid(const char *path, size_t size);

/* Authenticate exact detached bytes under the caller's configured Domain/environment signer.
 * The RFC 5652 SM2 verifier must use detached mode and a code-signing trust policy. All inputs
 * are borrowed for this call. The returned manifest describes the expected archive entries;
 * the archive owner must verify the actual entries before accepting the package.
 * On success, *out is one allocation owned by the caller. Release it with free(). On failure
 * *out is NULL. EPROTO rejects invalid signed schema. EPERM rejects a scope or trust mismatch. */
int flowie_control_release_manifest_verify(const salts_crypto_sm2_cms_verify_config *signer,
    const char *domain, const char *environment, const void *manifest, size_t manifest_size,
    const void *signature, size_t signature_size, flowie_release_manifest_t **out);

/* Match the archive reader's actual paths, byte counts and SM3 digests to the authenticated
 * manifest, independent of entry order. Exclude manifest.json and manifest.p7s. The reader must
 * validate full path bytes before NUL termination, enumerate every entry and compute each digest
 * over the actual bytes through EOF. */
int flowie_control_release_manifest_check_files(const flowie_release_manifest_t *manifest,
    const flowie_release_manifest_file_t *files, size_t count);

#endif
