#ifndef FLOWIE_CONTROL_RELEASE_STORE_INTERNAL_H
#define FLOWIE_CONTROL_RELEASE_STORE_INTERNAL_H

#include "flowie_control_release_archive_internal.h"
#include "flowie_protocol_repository.h"
#include "flowie_security.h"
#include <shared/orm/orm.h>

typedef struct flowie_control_release_store_s flowie_control_release_store_t;

typedef struct flowie_control_release_store_limits_s {
  int64_t domain_count;
  int64_t domain_bytes;
  int64_t total_count;
  int64_t total_bytes;
} flowie_control_release_store_limits_t;

typedef struct flowie_control_release_receipt_s {
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  char name[FLOWIE_RELEASE_MAX_NAME + 1];
  int created;
} flowie_control_release_receipt_t;

typedef struct flowie_control_release_active_s {
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  uint8_t acl_digest[FLOWIE_RELEASE_DIGEST_BYTES];
  uint64_t generation;
  uint64_t acl_version;
} flowie_control_release_active_t;

enum { FLOWIE_RELEASE_PAGE_CAPACITY = 32 };

typedef struct flowie_control_release_entry_s {
  uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES];
  int64_t byte_count;
  int64_t admitted_at;
} flowie_control_release_entry_t;

/* Digest-ordered keyset pagination. A NULL cursor starts from the first release. */
int flowie_control_release_store_list(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *after,
    flowie_control_release_entry_t entries[FLOWIE_RELEASE_PAGE_CAPACITY],
    size_t *count, int *more);

/* Record validation against the exact base generation and configured
 * trust/resource policy revision. The caller owns candidate verification. */
int flowie_control_release_store_validation(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *digest,
    uint64_t generation, const char *revision);

/* Owns a database connection. Adds the immutable package tables without changing the existing
 * identity/ACL tables. Operations serialize writes in the database, including quota checks.
 * Use one configured limits policy for all writers to the same database. */
int flowie_control_release_store_open(const orm_config_t *database,
    const flowie_control_release_store_limits_t *limits, flowie_control_release_store_t **out);
void flowie_control_release_store_close(flowie_control_release_store_t *store);
int flowie_control_release_journal_open(const orm_config_t *database, flowie_control_release_store_t **out);
/* Require a file-backed main database for the SQLite ingress journal. */
int flowie_control_release_store_check_journal(flowie_control_release_store_t *store,
    const flowie_control_release_store_t *session_store, const orm_config_t *session_database);

/* Verify and store the exact supplied bytes. All input borrows are immutable through return.
 * The trusted signer reference and policy are supplied by the caller after scope authorization.
 * An unstored archive requires a signed name. A missing name returns ENOENT.
 * The receipt includes the verified name, or an empty string for a legacy manifest.
 * A second verified upload of the same digest returns the receipt with created == 0. */
int flowie_control_release_store_admit(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *signer_reference,
    const salts_crypto_sm2_cms_verify_config *signer, const void *bytes, size_t size,
    flowie_control_release_receipt_t *out);

/* Read the named package in its exact stored representation and check the digest. The caller
 * owns the returned bytes and frees them with free(). A missing scope or digest returns ENOENT. */
int flowie_control_release_store_read(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t digest[FLOWIE_RELEASE_DIGEST_BYTES],
    void **bytes, size_t *size);

/* A zero generation denotes an uninitialized Domain. */
int flowie_control_release_store_current(flowie_control_release_store_t *store,
    const char *environment, const char *domain, flowie_control_release_active_t *out);

/* Read-only admission before preparing external resources. The commit transaction rechecks
 * this admission. A replay sets out to the historical result. A new operation leaves it zero. */
int flowie_control_release_store_activation_check(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key,
    uint64_t expected_generation, const char *revision, const flowie_release_archive_t *candidate,
    flowie_control_release_active_t *out, int *replayed);

/* Commit a prepared candidate while the serving authority holds its admission gate. The caller
 * must verify the archive for this exact scope and prepare the full supported runtime first.
 * Idempotency identity is scoped to actor and Domain. A replay reports the original committed
 * value, which may be older than the current generation. Stale bases or generations return
 * ECANCELED, a missing or stale validation receipt EAUTH, and reusing a key for a
 * different operation EALREADY. */
int flowie_control_release_store_commit(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key,
    uint64_t expected_generation, const char *revision, const flowie_release_archive_t *candidate,
    const uint8_t *acl_digest, flowie_control_release_active_t *out, int *replayed);

enum { FLOWIE_RELEASE_EVENT_TOPIC_MAX = 256, FLOWIE_RELEASE_EVENT_PAYLOAD_MAX = 16 * 1024 };
typedef struct flowie_release_journal_scope_s {
  const char *environment;
  const char *domain;
  uint64_t generation;
  const uint8_t *digest;
} flowie_release_journal_scope_t;

typedef struct flowie_release_job_s {
  int64_t sequence;
  char principal[FLOWIE_SECURITY_ID_MAX + 1u];
  char topic[FLOWIE_RELEASE_EVENT_TOPIC_MAX + 1u];
  size_t size;
  unsigned char payload[FLOWIE_RELEASE_EVENT_PAYLOAD_MAX];
} flowie_release_job_t;

int flowie_control_release_enqueue(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, const char *principal, const char *topic,
    const void *bytes, size_t size, const uint8_t *exchange);
/* Receipts outlive the history row until the protocol owner has committed PUBREC. */
int flowie_control_release_admission_find(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *exchange, int *found);
int flowie_control_release_admission_forget(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *exchange);
/* Before starting MQTT, retire receipts with no reserved exchange in the protocol repository. */
int flowie_control_release_admissions_reconcile(flowie_control_release_store_t *store,
    const char *environment, const char *domain, flowie_protocol_repository_t *repository);
/* Reject a journal with unfinished work outside the selected signed release. */
int flowie_control_release_journal_verify(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope);
/* A single backend owner claims and finishes jobs for one generation. ENOENT means empty. */
int flowie_control_release_claim(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, flowie_release_job_t *job);
int flowie_control_release_finish(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int64_t sequence, int result);
int flowie_control_release_record_output(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int64_t sequence, const char *key,
    const char *sink, const void *bytes, size_t size);
int flowie_control_release_finish_output(flowie_control_release_store_t *store, const char *key,
    int result, unsigned int http_status);
int flowie_control_release_interrupt(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope);
int flowie_control_release_pending(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int *pending);

#endif
