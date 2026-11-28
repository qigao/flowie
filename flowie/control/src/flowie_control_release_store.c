#include "flowie_control_release_store_internal.h"
#include "flowie_control_release_access_internal.h"

#include "flowie_control_database_internal.h"
#include "flowie_control_validation_internal.h"

#include <core/error.h>
#include <core/uuid.h>
#include <crypto/sha256.h>
#include <threads/threads.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct flowie_control_release_store_s {
  flowie_control_database_t *database;
  flowie_control_release_store_limits_t limits;
  sync_mutex_t gate;
  int unusable;
};

static const char release_store_schema[] =
    "CREATE TABLE IF NOT EXISTS flowie_release_write_lock ("
    "singleton INTEGER PRIMARY KEY CHECK (singleton = 1));"
    "INSERT INTO flowie_release_write_lock(singleton) VALUES(1) ON CONFLICT DO NOTHING;"
    "CREATE TABLE IF NOT EXISTS flowie_domain_release ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, "
    "digest BYTEA NOT NULL CHECK (length(digest) = 32), "
    "manifest_sm3 BYTEA NOT NULL CHECK (length(manifest_sm3) = 32), "
    "base_digest BYTEA CHECK (base_digest IS NULL OR length(base_digest) = 32), "
    "signer_reference TEXT NOT NULL, signer_key BYTEA NOT NULL CHECK (length(signer_key) = 65), "
    "admitted_at BIGINT NOT NULL CHECK (admitted_at >= 0), "
    "byte_count BIGINT NOT NULL CHECK (byte_count > 0), "
    "package_bytes BYTEA NOT NULL CHECK (byte_count = length(package_bytes)), "
    "PRIMARY KEY (environment, domain_id, digest));"
    "CREATE TABLE IF NOT EXISTS flowie_domain_release_active ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, digest BYTEA NOT NULL, "
    "acl_digest BYTEA NOT NULL, generation BIGINT NOT NULL CHECK(generation > 0), "
    "acl_version BIGINT NOT NULL CHECK(acl_version > 0), PRIMARY KEY(environment, domain_id));"
    "CREATE TABLE IF NOT EXISTS flowie_domain_release_activation ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, actor TEXT NOT NULL, idem_key TEXT NOT NULL, "
    "expected_generation BIGINT NOT NULL, digest BYTEA NOT NULL, acl_digest BYTEA NOT NULL, "
    "generation BIGINT NOT NULL, acl_version BIGINT NOT NULL, "
    "PRIMARY KEY(environment, domain_id, actor, idem_key), UNIQUE(environment, domain_id, generation));"
    "CREATE TABLE IF NOT EXISTS flowie_domain_release_validation ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, digest BYTEA NOT NULL, "
    "base_generation BIGINT NOT NULL CHECK(base_generation >= 0), policy_revision TEXT NOT NULL, "
    "PRIMARY KEY(environment, domain_id, digest), "
    "FOREIGN KEY(environment, domain_id, digest) REFERENCES flowie_domain_release);";

static const char release_journal_schema[] =
    "CREATE TABLE IF NOT EXISTS flowie_rule_admission ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, exchange BYTEA NOT NULL CHECK(length(exchange)=16),"
    "PRIMARY KEY(environment,domain_id,exchange));"
    "CREATE TABLE IF NOT EXISTS flowie_rule_generation ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, generation BIGINT NOT NULL,"
    "digest BYTEA NOT NULL CHECK(length(digest)=32), sequence BIGINT NOT NULL CHECK(sequence>=0),"
    "PRIMARY KEY(environment,domain_id,generation));"
    "CREATE TABLE IF NOT EXISTS flowie_rule_event ("
    "environment TEXT NOT NULL, domain_id TEXT NOT NULL, generation BIGINT NOT NULL,"
    "sequence BIGINT NOT NULL, principal TEXT NOT NULL, topic TEXT NOT NULL, payload BYTEA NOT NULL,"
    "received_at BIGINT NOT NULL, started_at BIGINT, finished_at BIGINT, status INTEGER,"
    "PRIMARY KEY(environment,domain_id,generation,sequence));"
    "CREATE INDEX IF NOT EXISTS flowie_rule_event_pending ON flowie_rule_event"
    "(environment,domain_id,generation,finished_at,started_at,sequence);"
    "CREATE TABLE IF NOT EXISTS flowie_rule_output ("
    "output_key TEXT PRIMARY KEY, environment TEXT NOT NULL, domain_id TEXT NOT NULL, generation BIGINT NOT NULL,"
    "sequence BIGINT NOT NULL, sink TEXT NOT NULL, body BYTEA NOT NULL, recorded_at BIGINT NOT NULL,"
    "finished_at BIGINT, status INTEGER, http_status INTEGER, FOREIGN KEY(environment,domain_id,generation,sequence)"
    " REFERENCES flowie_rule_event(environment,domain_id,generation,sequence));"
    "CREATE INDEX IF NOT EXISTS flowie_rule_output_event "
    "ON flowie_rule_output(environment,domain_id,generation,sequence);";

static int flowie_release_db_result(int status) {
  switch (status) {
  case FLOWIE_CONTROL_DB_OK:
  case FLOWIE_CONTROL_DB_DONE: return SALTS_OK;
  case FLOWIE_CONTROL_DB_BUSY:
  case FLOWIE_CONTROL_DB_LOCKED: return SALTS_EBUSY;
  case FLOWIE_CONTROL_DB_NOMEM: return SALTS_ENOMEM;
  default: return SALTS_EIO;
  }
}

static int flowie_release_exec(flowie_control_release_store_t *store, const char *sql) {
  return flowie_control_database_exec(store->database, sql, NULL, NULL, NULL);
}

static int flowie_release_lock_writer(flowie_control_release_store_t *store) {
  const int rc = flowie_release_db_result(flowie_release_exec(store,
      "UPDATE flowie_release_write_lock SET singleton=1 WHERE singleton=1"));
  if (rc != SALTS_OK) return rc;
  return flowie_control_database_changes(store->database) == 1 ? SALTS_OK : SALTS_EIO;
}

static int flowie_release_transaction_end(flowie_control_release_store_t *store, int result) {
  if (result == SALTS_OK) {
    result = flowie_release_db_result(flowie_release_exec(store, "COMMIT"));
    if (result == SALTS_OK) return result;
  }
  if (flowie_release_exec(store, "ROLLBACK") != FLOWIE_CONTROL_DB_OK) store->unusable = 1;
  return result;
}

static int flowie_release_store_open(const orm_config_t *database,
    const flowie_control_release_store_limits_t *limits, const char *schema,
    flowie_control_release_store_t **out) {
  if (!out) return SALTS_EINVAL;
  *out = NULL;
  if (!database) return SALTS_EINVAL;
  flowie_control_release_store_t *store = calloc(1u, sizeof(*store));
  if (!store) return SALTS_ENOMEM;
  sync_mutex_init(&store->gate);
  if (limits) store->limits = *limits;
  int status = flowie_control_database_open(database, &store->database);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_exec(store, "BEGIN");
  int rc = flowie_release_db_result(status);
  if (rc == SALTS_OK) {
    rc = flowie_release_db_result(flowie_release_exec(store, schema));
    rc = flowie_release_transaction_end(store, rc);
  }
  if (rc != SALTS_OK) {
    flowie_control_release_store_close(store);
    return rc;
  }
  *out = store;
  return SALTS_OK;
}

int flowie_control_release_store_open(const orm_config_t *database,
    const flowie_control_release_store_limits_t *limits, flowie_control_release_store_t **out) {
  if (out) *out = NULL;
  if (!limits || limits->domain_count <= 0 || limits->domain_bytes <= 0 ||
      limits->total_count < limits->domain_count || limits->total_bytes < limits->domain_bytes)
    return SALTS_EINVAL;
  return flowie_release_store_open(database, limits, release_store_schema, out);
}

int flowie_control_release_journal_open(const orm_config_t *database, flowie_control_release_store_t **out) {
  return flowie_release_store_open(database, NULL, release_journal_schema, out);
}

void flowie_control_release_store_close(flowie_control_release_store_t *store) {
  if (!store) return;
  flowie_control_database_close(store->database);
  sync_mutex_destroy(&store->gate);
  free(store);
}

static int journal_database_path(const flowie_control_release_store_t *store, char *path, size_t capacity) {
  static const char query[] = "SELECT file FROM pragma_database_list WHERE name='main'";
  /* Called during server preparation, with no other user of this connection. */
  flowie_control_statement_t *stmt = NULL;
  int status = flowie_control_database_prepare(store->database, query, -1, &stmt, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  int rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_EIO : flowie_release_db_result(status);
  if (status == FLOWIE_CONTROL_DB_ROW) {
    const int size = flowie_control_database_column_bytes(stmt, 0);
    if (size < 0 || (size_t)size >= capacity) rc = SALTS_ERANGE;
    else {
      if (size) memcpy(path, flowie_control_database_column_text(stmt, 0), (size_t)size);
      path[size] = '\0';
      rc = SALTS_OK;
    }
  }
  flowie_control_database_finalize(stmt);
  return rc;
}

int flowie_control_release_store_check_journal(flowie_control_release_store_t *store,
    const flowie_control_release_store_t *session_store, const orm_config_t *session_database) {
  char journal[4096], sessions[4096];
  int rc = journal_database_path(store, journal, sizeof(journal));
  if (rc != SALTS_OK) return rc;
  if (!journal[0]) return SALTS_EINVAL;
  static const char sqlite[] = "sqlite";
  if (!session_store || session_database->driver.len != sizeof(sqlite) - 1u ||
      memcmp(session_database->driver.data, sqlite, sizeof(sqlite) - 1u) != 0) return SALTS_OK;
  rc = journal_database_path(session_store, sessions, sizeof(sessions));
  if (rc != SALTS_OK) return rc;
#ifdef _WIN32
  return _stricmp(journal, sessions) == 0 ? SALTS_EINVAL : SALTS_OK;
#else
  return strcmp(journal, sessions) == 0 ? SALTS_EINVAL : SALTS_OK;
#endif
}

static int flowie_release_bind_key(flowie_control_statement_t *statement, const char *environment,
                                   const char *domain, const uint8_t *digest) {
  int status = flowie_control_database_bind_text(statement, 1, environment, -1,
                                                 FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 2, domain, -1,
                                               FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK && digest)
    status = flowie_control_database_bind_blob(statement, 3, digest, FLOWIE_RELEASE_DIGEST_BYTES,
                                               FLOWIE_CONTROL_DB_TRANSIENT);
  return status;
}

static int flowie_release_find(flowie_control_release_store_t *store, const char *sql,
    const char *environment, const char *domain, const uint8_t *digest,
    flowie_control_statement_t **statement) {
  int status = flowie_control_database_prepare(store->database, sql, -1, statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_release_bind_key(*statement, environment, domain, digest);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(*statement);
  return status;
}

static int flowie_release_check_quota(flowie_control_release_store_t *store,
    const char *environment, const char *domain, int64_t bytes) {
  static const char query[] =
      "SELECT COUNT(*), CAST(COALESCE(SUM(byte_count),0) AS BIGINT), "
      "(SELECT COUNT(*) FROM flowie_domain_release WHERE environment=? AND domain_id=?), "
      "CAST(COALESCE((SELECT SUM(byte_count) FROM flowie_domain_release "
      "WHERE environment=? AND domain_id=?),0) AS BIGINT) FROM flowie_domain_release";
  flowie_control_statement_t *statement = NULL;
  int status = flowie_control_database_prepare(store->database, query, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_bind_key(statement, environment, domain, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 3, environment, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 4, domain, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  int rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_EIO : flowie_release_db_result(status);
  if (status == FLOWIE_CONTROL_DB_ROW) {
    const int64_t total_count = flowie_control_database_column_int64(statement, 0);
    const int64_t total_bytes = flowie_control_database_column_int64(statement, 1);
    const int64_t domain_count = flowie_control_database_column_int64(statement, 2);
    const int64_t domain_bytes = flowie_control_database_column_int64(statement, 3);
    rc = total_count >= store->limits.total_count || domain_count >= store->limits.domain_count ||
         total_bytes > store->limits.total_bytes - bytes || domain_bytes > store->limits.domain_bytes - bytes
             ? SALTS_ENOSPC : SALTS_OK;
  }
  flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_release_insert(flowie_control_release_store_t *store, const char *environment,
    const char *domain, const char *signer_reference, const salts_crypto_sm2_trust_policy *trust,
    const flowie_release_archive_t *archive, const void *bytes, size_t size) {
  static const char query[] = "INSERT INTO flowie_domain_release "
      "(environment, domain_id, digest, manifest_sm3, base_digest, signer_reference, signer_key, "
      "admitted_at, byte_count, package_bytes) VALUES (?,?,?,?,?,?,?,?,?,?)";
  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(archive);
  flowie_control_statement_t *statement = NULL;
  int status = flowie_control_database_prepare(store->database, query, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_release_bind_key(statement, environment, domain,
                                     flowie_control_release_archive_digest(archive));
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_blob(statement, 4, manifest->sm3,
                                               sizeof(manifest->sm3), FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = manifest->has_base_release
        ? flowie_control_database_bind_blob(statement, 5, manifest->base_release,
            sizeof(manifest->base_release), FLOWIE_CONTROL_DB_TRANSIENT)
        : flowie_control_database_bind_null(statement, 5);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 6, signer_reference, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_blob(statement, 7, trust->expected_leaf_public_key,
                                               65, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 8, trust->validation_time);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 9, (int64_t)size);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_blob(statement, 10, bytes, (int)size, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  flowie_control_database_finalize(statement);
  return flowie_release_db_result(status);
}

static int flowie_release_store_verified(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *signer_reference,
    const salts_crypto_sm2_trust_policy *trust, const flowie_release_archive_t *archive,
    const void *bytes, size_t size, int *created) {
  int rc = flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
  if (rc != SALTS_OK) return rc;

  rc = flowie_release_lock_writer(store);
  if (rc == SALTS_OK) {
    flowie_control_statement_t *statement = NULL;
    const int found = flowie_release_find(store, "SELECT 1 FROM flowie_domain_release "
        "WHERE environment=? AND domain_id=? AND digest=?", environment, domain,
        flowie_control_release_archive_digest(archive), &statement);
    flowie_control_database_finalize(statement);
    if (found == FLOWIE_CONTROL_DB_DONE) {
      /* Previously stored unnamed packages can still be replayed by digest. */
      rc = flowie_control_release_archive_manifest(archive)->name[0] ?
          flowie_release_check_quota(store, environment, domain, (int64_t)size) : SALTS_ENOENT;
      if (rc == SALTS_OK) {
        rc = flowie_release_insert(store, environment, domain, signer_reference, trust,
                                   archive, bytes, size);
        if (rc == SALTS_OK) *created = 1;
      }
    } else if (found != FLOWIE_CONTROL_DB_ROW) {
      rc = flowie_release_db_result(found);
    }
  }
  return flowie_release_transaction_end(store, rc);
}

int flowie_control_release_store_admit(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *signer_reference,
    const salts_crypto_sm2_cms_verify_config *signer, const void *bytes, size_t size,
    flowie_control_release_receipt_t *out) {
  if (!out) return SALTS_EINVAL;
  *out = (flowie_control_release_receipt_t){0};
  if (!store || !flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(signer_reference, FLOWIE_SECURITY_ID_MAX)) return SALTS_EINVAL;
  flowie_release_archive_t *archive = NULL;
  int rc = flowie_control_release_archive_verify(signer, domain, environment, bytes, size, &archive);
  if (rc != SALTS_OK) return rc;
  flowie_control_release_receipt_t receipt = {0};
  sync_mutex_lock(&store->gate);
  rc = store->unusable ? SALTS_ESHUTDOWN : flowie_release_store_verified(store, environment, domain,
      signer_reference, signer->trust, archive, bytes, size, &receipt.created);
  sync_mutex_unlock(&store->gate);
  if (rc == SALTS_OK) {
    memcpy(receipt.digest, flowie_control_release_archive_digest(archive), sizeof(receipt.digest));
    memcpy(receipt.name, flowie_control_release_archive_manifest(archive)->name, sizeof(receipt.name));
    *out = receipt;
  }
  flowie_control_release_archive_destroy(archive);
  return rc;
}

int flowie_control_release_store_read(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *digest, void **bytes, size_t *size) {
  if (!bytes || !size) return SALTS_EINVAL;
  *bytes = NULL;
  *size = 0;
  if (!store || !digest || !flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX)) return SALTS_EINVAL;
  sync_mutex_lock(&store->gate);
  if (store->unusable) {
    sync_mutex_unlock(&store->gate);
    return SALTS_ESHUTDOWN;
  }
  flowie_control_statement_t *statement = NULL;
  const int status = flowie_release_find(store, "SELECT byte_count, package_bytes FROM "
      "flowie_domain_release WHERE environment=? AND domain_id=? AND digest=?",
      environment, domain, digest, &statement);
  int rc = status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT : flowie_release_db_result(status);
  size_t length = 0;
  void *copy = NULL;
  if (status == FLOWIE_CONTROL_DB_ROW) {
    const int64_t count = flowie_control_database_column_int64(statement, 0);
    const void *data = flowie_control_database_column_blob(statement, 1);
    rc = SALTS_EIO;
    if (data && count > 0 && count <= FLOWIE_RELEASE_MAX_COMPRESSED_BYTES &&
        flowie_control_database_column_bytes(statement, 1) == count) {
      length = (size_t)count;
      copy = malloc(length);
      rc = copy ? SALTS_OK : SALTS_ENOMEM;
      if (copy) memcpy(copy, data, length);
    }
  }
  flowie_control_database_finalize(statement);
  sync_mutex_unlock(&store->gate);
  if (rc != SALTS_OK) return rc;
  uint8_t computed[FLOWIE_RELEASE_DIGEST_BYTES];
  if (salts_crypto_sha256(copy, length, computed) != SALTS_CRYPTO_OK ||
      memcmp(computed, digest, sizeof(computed)) != 0) {
    free(copy);
    return SALTS_EIO;
  }
  *bytes = copy;
  *size = length;
  return SALTS_OK;
}

int flowie_control_release_store_list(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *after,
    flowie_control_release_entry_t entries[FLOWIE_RELEASE_PAGE_CAPACITY], size_t *count, int *more) {
  if (!store || !entries || !count || !more || !flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX)) return SALTS_EINVAL;
  *count = 0;
  *more = 0;
  sync_mutex_lock(&store->gate);
  if (store->unusable) {
    sync_mutex_unlock(&store->gate);
    return SALTS_ESHUTDOWN;
  }
  const char *sql = after
      ? "SELECT digest, byte_count, admitted_at FROM flowie_domain_release "
        "WHERE environment=? AND domain_id=? AND digest>? ORDER BY digest"
      : "SELECT digest, byte_count, admitted_at FROM flowie_domain_release "
        "WHERE environment=? AND domain_id=? ORDER BY digest";
  flowie_control_statement_t *statement = NULL;
  int status = flowie_release_find(store, sql, environment, domain, after, &statement);
  int rc = SALTS_OK;
  while (status == FLOWIE_CONTROL_DB_ROW && *count < FLOWIE_RELEASE_PAGE_CAPACITY) {
    const void *digest = flowie_control_database_column_blob(statement, 0);
    if (!digest || flowie_control_database_column_bytes(statement, 0) != FLOWIE_RELEASE_DIGEST_BYTES) {
      rc = SALTS_EIO;
      break;
    }
    flowie_control_release_entry_t *entry = &entries[(*count)++];
    memcpy(entry->digest, digest, sizeof(entry->digest));
    entry->byte_count = flowie_control_database_column_int64(statement, 1);
    entry->admitted_at = flowie_control_database_column_int64(statement, 2);
    status = flowie_control_database_step(statement);
  }
  if (rc == SALTS_OK) {
    *more = status == FLOWIE_CONTROL_DB_ROW;
    if (!*more) rc = flowie_release_db_result(status);
  }
  flowie_control_database_finalize(statement);
  sync_mutex_unlock(&store->gate);
  return rc;
}

static int flowie_release_validation_locked(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *digest,
    uint64_t generation, const char *revision, int record) {
  const char *sql = record
      ? "INSERT INTO flowie_domain_release_validation "
        "(environment, domain_id, digest, base_generation, policy_revision) VALUES(?,?,?,?,?) "
        "ON CONFLICT(environment, domain_id,digest) DO UPDATE SET "
        "base_generation=excluded.base_generation, policy_revision=excluded.policy_revision"
      : "SELECT 1 FROM flowie_domain_release_validation WHERE environment=? AND domain_id=? "
        "AND digest=? AND base_generation=? AND policy_revision=?";
  flowie_control_statement_t *statement = NULL;
  int status = flowie_control_database_prepare(store->database, sql, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_release_bind_key(statement, environment, domain, digest);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 4, (int64_t)generation);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, 5, revision, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  const int rc = record ? flowie_release_db_result(status)
      : status == FLOWIE_CONTROL_DB_ROW ? SALTS_OK
      : status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT : flowie_release_db_result(status);
  flowie_control_database_finalize(statement);
  return rc;
}

int flowie_control_release_store_validation(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *digest,
    uint64_t generation, const char *revision) {
  if (!store || !digest || generation > INT64_MAX ||
      !flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(revision, FLOWIE_SECURITY_ID_MAX)) return SALTS_EINVAL;
  sync_mutex_lock(&store->gate);
  const int rc = store->unusable ? SALTS_ESHUTDOWN :
      flowie_release_validation_locked(store, environment, domain, digest, generation, revision, 1);
  sync_mutex_unlock(&store->gate);
  return rc;
}

static int flowie_release_read_active(flowie_control_statement_t *statement,
                                      flowie_control_release_active_t *out) {
  const void *digest = flowie_control_database_column_blob(statement, 0);
  const void *acl = flowie_control_database_column_blob(statement, 1);
  const int64_t generation = flowie_control_database_column_int64(statement, 2);
  const int64_t version = flowie_control_database_column_int64(statement, 3);
  if (!digest || !acl || generation <= 0 || version <= 0 || version > generation ||
      flowie_control_database_column_bytes(statement, 0) != FLOWIE_RELEASE_DIGEST_BYTES ||
      flowie_control_database_column_bytes(statement, 1) != FLOWIE_RELEASE_DIGEST_BYTES)
    return SALTS_EIO;
  memcpy(out->digest, digest, sizeof(out->digest));
  memcpy(out->acl_digest, acl, sizeof(out->acl_digest));
  out->generation = (uint64_t)generation;
  out->acl_version = (uint64_t)version;
  return SALTS_OK;
}

static int flowie_release_current_locked(flowie_control_release_store_t *store,
    const char *environment, const char *domain, flowie_control_release_active_t *out) {
  flowie_control_statement_t *statement = NULL;
  const int status = flowie_release_find(store, "SELECT digest, acl_digest, generation, acl_version "
      "FROM flowie_domain_release_active WHERE environment=? AND domain_id=?",
      environment, domain, NULL, &statement);
  *out = (flowie_control_release_active_t){0};
  const int rc = status == FLOWIE_CONTROL_DB_ROW ? flowie_release_read_active(statement, out)
                                                  : flowie_release_db_result(status);
  flowie_control_database_finalize(statement);
  return rc;
}

int flowie_control_release_store_current(flowie_control_release_store_t *store,
    const char *environment, const char *domain, flowie_control_release_active_t *out) {
  if (!store || !out || !flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) ||
      !flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX)) return SALTS_EINVAL;
  sync_mutex_lock(&store->gate);
  const int rc = store->unusable ? SALTS_ESHUTDOWN :
      flowie_release_current_locked(store, environment, domain, out);
  sync_mutex_unlock(&store->gate);
  return rc;
}

static int flowie_release_bind_identity(flowie_control_statement_t *statement, int index,
                                        const char *actor, const char *key) {
  int status = flowie_control_database_bind_text(statement, index, actor, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_text(statement, index + 1, key, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  return status;
}

static int flowie_release_replay(flowie_control_release_store_t *store, const char *environment,
    const char *domain, const char *actor, const char *key, const uint8_t *digest,
    uint64_t expected, flowie_control_release_active_t *out, int *replayed) {
  static const char query[] = "SELECT digest, acl_digest, generation, acl_version, expected_generation "
      "FROM flowie_domain_release_activation WHERE environment=? AND domain_id=? AND actor=? AND idem_key=?";
  flowie_control_statement_t *statement = NULL;
  int status = flowie_control_database_prepare(store->database, query, -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_bind_key(statement, environment, domain, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_bind_identity(statement, 3, actor, key);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  int rc = flowie_release_db_result(status);
  if (status == FLOWIE_CONTROL_DB_ROW) {
    rc = flowie_release_read_active(statement, out);
    if (rc == SALTS_OK) {
      if (memcmp(digest, out->digest, sizeof(out->digest)) != 0 ||
          flowie_control_database_column_int64(statement, 4) != (int64_t)expected)
        rc = SALTS_EALREADY;
      else
        *replayed = 1;
    }
  }
  flowie_control_database_finalize(statement);
  return rc;
}

static int flowie_release_write_active(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const flowie_control_release_active_t *active,
    const char *actor, const char *key, uint64_t expected) {
  static const char upsert[] = "INSERT INTO flowie_domain_release_active "
      "(environment, domain_id, digest, acl_digest, generation, acl_version) VALUES (?,?,?,?,?,?) "
      "ON CONFLICT(environment, domain_id) DO UPDATE SET digest=excluded.digest, "
      "acl_digest=excluded.acl_digest, generation=excluded.generation, acl_version=excluded.acl_version";
  static const char audit[] = "INSERT INTO flowie_domain_release_activation (environment, domain_id, "
      "digest, acl_digest, generation, acl_version, actor, idem_key, expected_generation) "
      "VALUES (?,?,?,?,?,?,?,?,?)";
  flowie_control_statement_t *statement = NULL;
  int status = flowie_control_database_prepare(store->database, actor ? audit : upsert,
                                              -1, &statement, NULL);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_release_bind_key(statement, environment, domain, active->digest);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_blob(statement, 4, active->acl_digest,
        sizeof(active->acl_digest), FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 5, (int64_t)active->generation);
  if (status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 6, (int64_t)active->acl_version);
  if (actor && status == FLOWIE_CONTROL_DB_OK)
    status = flowie_release_bind_identity(statement, 7, actor, key);
  if (actor && status == FLOWIE_CONTROL_DB_OK)
    status = flowie_control_database_bind_int64(statement, 9, (int64_t)expected);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(statement);
  flowie_control_database_finalize(statement);
  return flowie_release_db_result(status);
}

static int flowie_release_check_activation_locked(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key, uint64_t expected,
    const char *revision, const flowie_release_archive_t *candidate,
    flowie_control_release_active_t *active, flowie_control_release_active_t *out, int *replayed) {
  const uint8_t *digest = flowie_control_release_archive_digest(candidate);
  const flowie_release_manifest_t *manifest = flowie_control_release_archive_manifest(candidate);
  int rc = flowie_release_replay(store, environment, domain, actor, key, digest, expected, out, replayed);
  if (rc != SALTS_OK || *replayed) return rc;
  rc = flowie_release_current_locked(store, environment, domain, active);
  if (rc != SALTS_OK) return rc;
  if (expected != active->generation || manifest->has_base_release != (active->generation != 0) ||
      (active->generation && memcmp(manifest->base_release, active->digest, sizeof(active->digest)) != 0))
    return SALTS_ECANCELED;
  if (active->generation == INT64_MAX) return SALTS_ERANGE;

  flowie_control_statement_t *statement = NULL;
  const int found = flowie_release_find(store, "SELECT 1 FROM flowie_domain_release WHERE "
      "environment=? AND domain_id=? AND digest=?", environment, domain, digest, &statement);
  flowie_control_database_finalize(statement);
  if (found != FLOWIE_CONTROL_DB_ROW)
    return found == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT : flowie_release_db_result(found);
  rc = flowie_release_validation_locked(store, environment, domain, digest, expected, revision, 0);
  return rc == SALTS_ENOENT ? SALTS_EAUTH : rc;
}

static int flowie_release_promote_locked(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key, uint64_t expected,
    const char *revision, const flowie_release_archive_t *candidate, const uint8_t *acl_digest,
    flowie_control_release_active_t *out, int *replayed) {
  int rc = flowie_release_lock_writer(store);
  if (rc != SALTS_OK) return rc;
  flowie_control_release_active_t active;
  rc = flowie_release_check_activation_locked(store, environment, domain, actor, key, expected,
      revision, candidate, &active, out, replayed);
  if (rc != SALTS_OK || *replayed) return rc;

  if (!active.generation || memcmp(active.acl_digest, acl_digest, sizeof(active.acl_digest)) != 0)
    ++active.acl_version;
  ++active.generation;
  memcpy(active.digest, flowie_control_release_archive_digest(candidate), sizeof(active.digest));
  memcpy(active.acl_digest, acl_digest, sizeof(active.acl_digest));
  rc = flowie_release_write_active(store, environment, domain, &active, NULL, NULL, expected);
  if (rc == SALTS_OK) rc = flowie_release_write_active(store, environment, domain, &active, actor, key, expected);
  if (rc == SALTS_OK) *out = active;
  return rc;
}

static int flowie_release_activation_valid(const char *environment, const char *domain,
    const char *actor, const char *key, uint64_t expected, const char *revision) {
  return expected <= INT64_MAX && flowie_control_text_valid(environment, FLOWIE_SECURITY_ID_MAX) &&
      flowie_control_text_valid(domain, FLOWIE_SECURITY_ID_MAX) &&
      flowie_control_text_valid(actor, FLOWIE_RELEASE_ACCESS_ACTOR_MAX) &&
      flowie_control_text_valid(revision, FLOWIE_SECURITY_ID_MAX) &&
      flowie_control_text_valid(key, FLOWIE_SECURITY_ID_MAX);
}

int flowie_control_release_store_activation_check(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key,
    uint64_t expected_generation, const char *revision, const flowie_release_archive_t *candidate,
    flowie_control_release_active_t *out, int *replayed) {
  if (!store || !candidate || !out || !replayed ||
      !flowie_release_activation_valid(environment, domain, actor, key, expected_generation, revision))
    return SALTS_EINVAL;
  flowie_control_release_active_t current, historical = {0};
  *out = (flowie_control_release_active_t){0};
  *replayed = 0;
  int replay = 0;
  sync_mutex_lock(&store->gate);
  const int rc = store->unusable ? SALTS_ESHUTDOWN : flowie_release_check_activation_locked(store,
      environment, domain, actor, key, expected_generation, revision, candidate, &current,
      &historical, &replay);
  sync_mutex_unlock(&store->gate);
  if (rc == SALTS_OK && replay) {
    *out = historical;
    *replayed = 1;
  }
  return rc;
}

int flowie_control_release_store_commit(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const char *actor, const char *key,
    uint64_t expected_generation, const char *revision, const flowie_release_archive_t *candidate,
    const uint8_t *acl_digest, flowie_control_release_active_t *out, int *replayed) {
  if (!store || !candidate || !acl_digest || !out || !replayed ||
      !flowie_release_activation_valid(environment, domain, actor, key, expected_generation, revision))
    return SALTS_EINVAL;
  *out = (flowie_control_release_active_t){0};
  *replayed = 0;
  flowie_control_release_active_t result = {0};
  int replay = 0;
  sync_mutex_lock(&store->gate);
  int rc = store->unusable ? SALTS_ESHUTDOWN :
      flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
  if (rc == SALTS_OK) {
    rc = flowie_release_promote_locked(store, environment, domain, actor, key, expected_generation,
                                       revision, candidate, acl_digest, &result, &replay);
    rc = flowie_release_transaction_end(store, rc);
  }
  sync_mutex_unlock(&store->gate);
  if (rc == SALTS_OK) {
    *out = result;
    *replayed = replay;
  }
  return rc;
}

enum { FLOWIE_RULE_PENDING_MAX = 256, FLOWIE_RULE_HISTORY_MAX = 8192 };

static int journal_scope(flowie_control_release_store_t *store, const char *sql,
    const flowie_release_journal_scope_t *scope, flowie_control_statement_t **out) {
  if (store->unusable) return FLOWIE_CONTROL_DB_ERROR;
  int rc = flowie_control_database_prepare(store->database, sql, -1, out, NULL);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_release_bind_key(*out, scope->environment, scope->domain, NULL);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int64(*out, 3, (int64_t)scope->generation);
  return rc;
}

static int journal_write(flowie_control_release_store_t *store, flowie_control_statement_t *stmt,
    int status, int unchanged) {
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  flowie_control_database_finalize(stmt);
  int rc = flowie_release_db_result(status);
  if (rc == SALTS_OK && flowie_control_database_changes(store->database) != 1) rc = unchanged;
  return rc;
}

static int journal_verify_locked(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope) {
  static const char sql[] =
      "SELECT 1 FROM flowie_rule_generation WHERE environment=?1 AND domain_id=?2 "
      "AND generation=?3 AND digest<>?4 "
      "UNION ALL SELECT 1 FROM flowie_rule_event AS e WHERE e.environment=?1 AND e.domain_id=?2 "
      "AND (e.finished_at IS NULL OR EXISTS (SELECT 1 FROM flowie_rule_output AS o "
      "WHERE o.environment=e.environment AND o.domain_id=e.domain_id AND o.generation=e.generation "
      "AND o.sequence=e.sequence AND o.finished_at IS NULL)) "
      "AND (e.generation<>?3 OR NOT EXISTS (SELECT 1 FROM flowie_rule_generation AS g "
      "WHERE g.environment=?1 AND g.domain_id=?2 AND g.generation=?3 AND g.digest=?4)) LIMIT 1";
  flowie_control_statement_t *stmt = NULL;
  int status = journal_scope(store, sql, scope, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_blob(stmt, 4,
      scope->digest, FLOWIE_RELEASE_DIGEST_BYTES, FLOWIE_CONTROL_DB_TRANSIENT);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  flowie_control_database_finalize(stmt);
  return status == FLOWIE_CONTROL_DB_ROW ? SALTS_EPROTO : flowie_release_db_result(status);
}

int flowie_control_release_journal_verify(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope) {
  sync_mutex_lock(&store->gate);
  const int rc = journal_verify_locked(store, scope);
  sync_mutex_unlock(&store->gate);
  return rc;
}

static int journal_prune(flowie_control_release_store_t *store, const char *sql,
    const flowie_release_journal_scope_t *scope) {
  flowie_control_statement_t *stmt = NULL;
  int status = journal_scope(store, sql, scope, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 4, FLOWIE_RULE_HISTORY_MAX);
  return journal_write(store, stmt, status, SALTS_OK);
}

static int journal_next_sequence(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope) {
  static const char sql[] =
      "INSERT INTO flowie_rule_generation(environment,domain_id,generation,digest,sequence) "
      "SELECT ?1,?2,?3,?4,sequence+1 FROM (SELECT COALESCE(MAX(sequence),0) AS sequence "
      "FROM flowie_rule_event WHERE environment=?1 AND domain_id=?2 AND generation=?3) "
      "WHERE sequence<9223372036854775807 "
      "ON CONFLICT(environment,domain_id,generation) DO UPDATE SET sequence=sequence+1 "
      "WHERE digest=excluded.digest AND sequence<9223372036854775807";
  flowie_control_statement_t *stmt = NULL;
  int status = journal_scope(store, sql, scope, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_blob(stmt, 4,
      scope->digest, FLOWIE_RELEASE_DIGEST_BYTES, FLOWIE_CONTROL_DB_TRANSIENT);
  return journal_write(store, stmt, status, SALTS_EPROTO);
}

static int journal_admission(flowie_control_release_store_t *store, const char *sql,
    const char *environment, const char *domain, const uint8_t *exchange,
    flowie_control_statement_t **stmt) {
  int status = store->unusable ? FLOWIE_CONTROL_DB_ERROR :
      flowie_control_database_prepare(store->database, sql, -1, stmt, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_bind_key(*stmt, environment, domain, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_blob(*stmt, 3,
      exchange, SALTS_UUID_SIZE, FLOWIE_CONTROL_DB_TRANSIENT);
  return status;
}

static int journal_admission_find_locked(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *exchange, int *found) {
  static const char sql[] = "SELECT 1 FROM flowie_rule_admission WHERE environment=?1 "
      "AND domain_id=?2 AND exchange=?3";
  flowie_control_statement_t *stmt = NULL;
  int status = journal_admission(store, sql, environment, domain, exchange, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  flowie_control_database_finalize(stmt);
  *found = status == FLOWIE_CONTROL_DB_ROW;
  return *found ? SALTS_OK : flowie_release_db_result(status);
}

int flowie_control_release_admission_find(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *exchange, int *found) {
  sync_mutex_lock(&store->gate);
  const int rc = journal_admission_find_locked(store, environment, domain, exchange, found);
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_admission_forget(flowie_control_release_store_t *store,
    const char *environment, const char *domain, const uint8_t *exchange) {
  static const char sql[] = "DELETE FROM flowie_rule_admission WHERE environment=?1 "
      "AND domain_id=?2 AND exchange=?3";
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  const int status = journal_admission(store, sql, environment, domain, exchange, &stmt);
  const int rc = journal_write(store, stmt, status, SALTS_OK);
  sync_mutex_unlock(&store->gate);
  return rc;
}

typedef struct flowie_release_admission_s {
  uuid_t exchange;
  int reserved;
} flowie_release_admission_t;

typedef struct flowie_release_admissions_s {
  flowie_release_admission_t *rows;
  size_t count;
} flowie_release_admissions_t;

static int journal_compare_exchange(const void *key, const void *entry) {
  const flowie_release_admission_t *receipt = entry;
  return memcmp(key, receipt->exchange, sizeof(receipt->exchange));
}

static int journal_keep_pending_exchange(void *context, const flowie_protocol_session_row_t *row) {
  flowie_release_admissions_t *receipts = context;
  for (size_t i = 0; i < row->inflight_count; ++i) {
    const uint8_t *id = row->inflight[i].exchange;
    if (!id[6]) continue;
    flowie_release_admission_t *receipt = bsearch(id, receipts->rows, receipts->count,
        sizeof(*receipts->rows), journal_compare_exchange);
    if (receipt) receipt->reserved = 1;
  }
  return SALTS_OK;
}

int flowie_control_release_admissions_reconcile(flowie_control_release_store_t *store,
    const char *environment, const char *domain, flowie_protocol_repository_t *repository) {
  static const char list[] = "SELECT exchange FROM flowie_rule_admission "
      "WHERE environment=?1 AND domain_id=?2 ORDER BY exchange";
  static const char retire[] = "DELETE FROM flowie_rule_admission "
      "WHERE environment=?1 AND domain_id=?2 AND exchange=?3";
  flowie_release_admissions_t receipts = {calloc(FLOWIE_RULE_HISTORY_MAX, sizeof(*receipts.rows)), 0};
  if (!receipts.rows) return SALTS_ENOMEM;
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  int status = store->unusable ? FLOWIE_CONTROL_DB_ERROR :
      flowie_control_database_prepare(store->database, list, -1, &stmt, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_release_bind_key(stmt, environment, domain, NULL);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  int rc = SALTS_OK;
  for (; status == FLOWIE_CONTROL_DB_ROW; status = flowie_control_database_step(stmt)) {
    const int bytes = flowie_control_database_column_bytes(stmt, 0);
    if (flowie_control_database_column_type(stmt, 0) != FLOWIE_CONTROL_DB_BLOB ||
        bytes != SALTS_UUID_SIZE || receipts.count == FLOWIE_RULE_HISTORY_MAX) {
      rc = SALTS_EPROTO;
      break;
    }
    memcpy(receipts.rows[receipts.count++].exchange,
        flowie_control_database_column_blob(stmt, 0), SALTS_UUID_SIZE);
  }
  flowie_control_database_finalize(stmt);
  if (rc == SALTS_OK) rc = flowie_release_db_result(status);
  /* The repository worker marks borrowed UUIDs in memory. It does not touch the journal. */
  if (rc == SALTS_OK && receipts.count)
    rc = flowie_protocol_repository_session_visit(repository, journal_keep_pending_exchange, &receipts);
  if (rc == SALTS_OK && receipts.count) {
    rc = flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
    if (rc == SALTS_OK) {
      for (size_t i = 0; i < receipts.count && rc == SALTS_OK; ++i) {
        if (receipts.rows[i].reserved) continue;
        stmt = NULL;
        status = journal_admission(store, retire, environment, domain, receipts.rows[i].exchange, &stmt);
        rc = journal_write(store, stmt, status, SALTS_OK);
      }
      rc = flowie_release_transaction_end(store, rc);
    }
  }
  sync_mutex_unlock(&store->gate);
  free(receipts.rows);
  return rc;
}

int flowie_control_release_enqueue(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, const char *principal, const char *topic,
    const void *bytes, size_t size, const uint8_t *exchange) {
  if (size > FLOWIE_RELEASE_EVENT_PAYLOAD_MAX) return SALTS_EMSGSIZE;
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
#define FLOWIE_RULE_OLDEST_EVENT \
      "FROM flowie_rule_event AS e WHERE e.environment=?1 AND e.domain_id=?2 AND ?3>0 AND e.finished_at IS NOT NULL " \
      "AND NOT EXISTS (SELECT 1 FROM flowie_rule_output AS o WHERE o.environment=e.environment " \
      "AND o.domain_id=e.domain_id AND o.generation=e.generation AND o.sequence=e.sequence " \
      "AND (o.finished_at IS NULL OR (o.status<>0 AND o.http_status IS NULL))) " \
      "AND (SELECT COUNT(*) FROM flowie_rule_event WHERE environment=?1 AND domain_id=?2)>=?4 " \
      "ORDER BY e.received_at,e.generation,e.sequence LIMIT 1)"
  static const char prune_event[] =
      "DELETE FROM flowie_rule_event WHERE rowid=(SELECT e.rowid " FLOWIE_RULE_OLDEST_EVENT;
  static const char prune_outputs[] =
      "DELETE FROM flowie_rule_output WHERE environment=?1 AND domain_id=?2 AND (generation,sequence)="
      "(SELECT e.generation,e.sequence " FLOWIE_RULE_OLDEST_EVENT;
#undef FLOWIE_RULE_OLDEST_EVENT
  static const char sql[] =
      "INSERT INTO flowie_rule_event(environment,domain_id,generation,sequence,principal,topic,payload,received_at) "
      "SELECT ?1,?2,?3,sequence,?4,?5,?6,?7 FROM flowie_rule_generation "
      "WHERE environment=?1 AND domain_id=?2 AND generation=?3 "
      "AND (SELECT COUNT(*) FROM flowie_rule_event WHERE environment=?1 AND domain_id=?2) < ?8 "
      "AND (SELECT COUNT(*) FROM flowie_rule_event WHERE environment=?1 AND domain_id=?2 "
      "AND generation=?3 AND finished_at IS NULL) < ?9";
  static const char admit[] = "INSERT INTO flowie_rule_admission(environment,domain_id,exchange) "
      "SELECT ?1,?2,?3 WHERE (SELECT COUNT(*) FROM flowie_rule_admission "
      "WHERE environment=?1 AND domain_id=?2)< ?4";
  sync_mutex_lock(&store->gate);
  int rc = store->unusable ? SALTS_EIO : flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
  if (rc != SALTS_OK) {
    sync_mutex_unlock(&store->gate);
    return rc;
  }
  int found = 0;
  if (exchange) {
    rc = journal_admission_find_locked(store, scope->environment, scope->domain, exchange, &found);
    if (rc == SALTS_OK && !found) {
      flowie_control_statement_t *receipt = NULL;
      int status = journal_admission(store, admit, scope->environment, scope->domain, exchange, &receipt);
      if (status == FLOWIE_CONTROL_DB_OK)
        status = flowie_control_database_bind_int(receipt, 4, FLOWIE_RULE_HISTORY_MAX);
      rc = journal_write(store, receipt, status, SALTS_ENOSPC);
    }
  }
  if (found) {
    rc = flowie_release_transaction_end(store, rc);
    sync_mutex_unlock(&store->gate);
    return rc;
  }
  if (rc == SALTS_OK) rc = journal_next_sequence(store, scope);
  if (rc == SALTS_OK) rc = journal_prune(store, prune_outputs, scope);
  if (rc == SALTS_OK) rc = journal_prune(store, prune_event, scope);
  flowie_control_statement_t *stmt = NULL;
  if (rc == SALTS_OK) {
    int status = journal_scope(store, sql, scope, &stmt);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_text(stmt, 4, principal, -1, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_text(stmt, 5, topic, -1, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_blob(stmt, 6, size ? bytes : "", (int)size, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 7, (int64_t)now);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 8, FLOWIE_RULE_HISTORY_MAX);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 9, FLOWIE_RULE_PENDING_MAX);
    rc = journal_write(store, stmt, status, SALTS_ENOSPC);
  }
  rc = flowie_release_transaction_end(store, rc);
  sync_mutex_unlock(&store->gate);
  return rc;
}

static int journal_read_job(const flowie_control_statement_t *stmt, flowie_release_job_t *job) {
  const int principal_size = flowie_control_database_column_bytes(stmt, 1);
  const int topic_size = flowie_control_database_column_bytes(stmt, 2);
  const int payload_size = flowie_control_database_column_bytes(stmt, 3);
  if (principal_size <= 0 || (size_t)principal_size >= sizeof(job->principal) ||
      topic_size <= 0 || (size_t)topic_size >= sizeof(job->topic) ||
      payload_size < 0 || (size_t)payload_size > sizeof(job->payload)) return SALTS_EPROTO;
  job->sequence = flowie_control_database_column_int64(stmt, 0);
  if (job->sequence <= 0) return SALTS_EPROTO;
  memcpy(job->principal, flowie_control_database_column_text(stmt, 1), (size_t)principal_size);
  job->principal[principal_size] = 0;
  memcpy(job->topic, flowie_control_database_column_text(stmt, 2), (size_t)topic_size);
  job->topic[topic_size] = 0;
  job->size = (size_t)payload_size;
  if (payload_size) memcpy(job->payload, flowie_control_database_column_blob(stmt, 3), job->size);
  return SALTS_OK;
}

int flowie_control_release_claim(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, flowie_release_job_t *job) {
  static const char query[] = "SELECT sequence,principal,topic,payload FROM flowie_rule_event "
      "WHERE environment=? AND domain_id=? AND generation=? AND started_at IS NULL "
      "AND finished_at IS NULL ORDER BY sequence LIMIT 1";
  static const char start[] = "UPDATE flowie_rule_event SET started_at=?4 "
      "WHERE environment=?1 AND domain_id=?2 AND generation=?3 AND sequence=?5 AND started_at IS NULL";
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  int status = journal_scope(store, query, scope, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  int rc = status == FLOWIE_CONTROL_DB_ROW ? journal_read_job(stmt, job) :
      status == FLOWIE_CONTROL_DB_DONE ? SALTS_ENOENT : flowie_release_db_result(status);
  flowie_control_database_finalize(stmt);
  if (rc == SALTS_OK) {
    stmt = NULL;
    status = journal_scope(store, start, scope, &stmt);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 4, (int64_t)now);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 5, job->sequence);
    rc = journal_write(store, stmt, status, SALTS_EIO);
  }
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_finish(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int64_t sequence, int result) {
  static const char sql[] = "UPDATE flowie_rule_event SET finished_at=COALESCE(finished_at,?4),status=?5 "
      "WHERE environment=?1 AND domain_id=?2 AND generation=?3 AND sequence=?6 "
      "AND started_at IS NOT NULL AND (finished_at IS NULL OR status=?5)";
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  int rc = journal_scope(store, sql, scope, &stmt);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int64(stmt, 4, (int64_t)now);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int(stmt, 5, result);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int64(stmt, 6, sequence);
  rc = journal_write(store, stmt, rc, SALTS_EIO);
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_record_output(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int64_t sequence, const char *key,
    const char *sink, const void *bytes, size_t size) {
  if (size > FLOWIE_RELEASE_EVENT_PAYLOAD_MAX) return SALTS_EMSGSIZE;
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
  static const char prune[] = "DELETE FROM flowie_rule_output WHERE rowid=(SELECT o.rowid "
      "FROM flowie_rule_output AS o JOIN flowie_rule_event AS e "
      "ON e.environment=o.environment AND e.domain_id=o.domain_id AND e.generation=o.generation "
      "AND e.sequence=o.sequence WHERE o.environment=?1 AND o.domain_id=?2 AND ?3>0 "
      "AND e.finished_at IS NOT NULL AND o.finished_at IS NOT NULL "
      "AND (o.status=0 OR o.http_status IS NOT NULL) ORDER BY o.recorded_at,o.rowid LIMIT 1) AND "
      "(SELECT COUNT(*) FROM flowie_rule_output WHERE environment=?1 AND domain_id=?2)>=?4";
  static const char sql[] = "WITH scope(e,d,g) AS (VALUES (CAST(? AS TEXT),CAST(? AS TEXT),CAST(? AS BIGINT))) "
      "INSERT INTO flowie_rule_output(environment,domain_id,generation,sequence,output_key,sink,body,recorded_at) "
      "SELECT e,d,g,?,?,?,?,? FROM scope WHERE (SELECT COUNT(*) FROM flowie_rule_output "
      "WHERE environment=e AND domain_id=d) < ?";
  sync_mutex_lock(&store->gate);
  int rc = store->unusable ? SALTS_EIO : flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
  if (rc != SALTS_OK) {
    sync_mutex_unlock(&store->gate);
    return rc;
  }
  rc = journal_prune(store, prune, scope);
  flowie_control_statement_t *stmt = NULL;
  if (rc == SALTS_OK) {
    int status = journal_scope(store, sql, scope, &stmt);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 4, sequence);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_text(stmt, 5, key, -1, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_text(stmt, 6, sink, -1, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_blob(stmt, 7, size ? bytes : "", (int)size, FLOWIE_CONTROL_DB_TRANSIENT);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 8, (int64_t)now);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 9, FLOWIE_RULE_HISTORY_MAX);
    rc = journal_write(store, stmt, status, SALTS_ENOSPC);
  }
  rc = flowie_release_transaction_end(store, rc);
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_finish_output(flowie_control_release_store_t *store, const char *key,
    int result, unsigned int http_status) {
  static const char sql[] = "UPDATE flowie_rule_output SET finished_at=COALESCE(finished_at,?1), "
      "status=?2,http_status=?3 WHERE output_key=?4 "
      "AND (finished_at IS NULL OR (status=?2 AND http_status IS ?3))";
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  int rc = store->unusable ? FLOWIE_CONTROL_DB_ERROR :
      flowie_control_database_prepare(store->database, sql, -1, &stmt, NULL);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int64(stmt, 1, (int64_t)now);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_int(stmt, 2, result);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = http_status ? flowie_control_database_bind_int(stmt, 3, (int)http_status) :
      flowie_control_database_bind_null(stmt, 3);
  if (rc == FLOWIE_CONTROL_DB_OK) rc = flowie_control_database_bind_text(stmt, 4, key, -1, FLOWIE_CONTROL_DB_TRANSIENT);
  rc = journal_write(store, stmt, rc, SALTS_EIO);
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_interrupt(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope) {
  static const char *const commands[] = {
      "UPDATE flowie_rule_output SET finished_at=?4,status=?5 WHERE environment=?1 AND domain_id=?2 "
      "AND generation=?3 AND finished_at IS NULL",
      "UPDATE flowie_rule_event SET finished_at=?4,status=?5 WHERE environment=?1 AND domain_id=?2 "
      "AND generation=?3 AND started_at IS NOT NULL AND finished_at IS NULL"};
  const time_t now = time(NULL);
  if (now < 0) return SALTS_EIO;
  sync_mutex_lock(&store->gate);
  int rc = store->unusable ? SALTS_EIO : flowie_release_db_result(flowie_release_exec(store, "BEGIN"));
  if (rc != SALTS_OK) {
    sync_mutex_unlock(&store->gate);
    return rc;
  }
  for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]) && rc == SALTS_OK; ++i) {
    flowie_control_statement_t *stmt = NULL;
    int status = journal_scope(store, commands[i], scope, &stmt);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int64(stmt, 4, (int64_t)now);
    if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_bind_int(stmt, 5, SALTS_ECANCELED);
    rc = journal_write(store, stmt, status, SALTS_OK);
  }
  rc = flowie_release_transaction_end(store, rc);
  sync_mutex_unlock(&store->gate);
  return rc;
}

int flowie_control_release_pending(flowie_control_release_store_t *store,
    const flowie_release_journal_scope_t *scope, int *pending) {
  static const char sql[] = "SELECT 1 FROM flowie_rule_event WHERE environment=?1 AND domain_id=?2 "
      "AND generation=?3 AND finished_at IS NULL UNION ALL SELECT 1 FROM flowie_rule_output "
      "WHERE environment=?1 AND domain_id=?2 AND generation=?3 AND finished_at IS NULL LIMIT 1";
  sync_mutex_lock(&store->gate);
  flowie_control_statement_t *stmt = NULL;
  int status = journal_scope(store, sql, scope, &stmt);
  if (status == FLOWIE_CONTROL_DB_OK) status = flowie_control_database_step(stmt);
  *pending = status == FLOWIE_CONTROL_DB_ROW;
  flowie_control_database_finalize(stmt);
  sync_mutex_unlock(&store->gate);
  return *pending ? SALTS_OK : flowie_release_db_result(status);
}
