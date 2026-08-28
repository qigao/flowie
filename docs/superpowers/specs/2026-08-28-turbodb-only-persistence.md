# TurboDB-Only Persistence Design

## Status

Approved by the user on 2026-08-28. This decision supersedes the staged legacy-provider
retention described by the earlier Control ORM migration design.

## Goal

Flowie must not compile, link, or call SQLite or PostgreSQL client APIs. TurboDB owns every
database connection, transaction, parameter binding, command result, row conversion, driver
diagnostic, and shutdown operation used by Flowie.

## Boundaries

- Control services continue to depend only on `flowie_control_repository_t`.
- Flowie persistence adapters depend only on the public TurboDB ORM C ABI.
- Flowie has one storage configuration block, `storage.turbodb`. It contains the TurboDB driver
  and bounded key/value options passed unchanged to `orm_connect`; Flowie has no SQLite or
  PostgreSQL configuration structs, providers, build switches, pools, commands, or queries.
- Domain, role, group, user, credential, ACL, publish, revision, and audit semantics remain
  unchanged. Management RPC request and response contracts remain unchanged.
- There is one selected fact source. Connection or validation failure is fatal; there is no
  backend fallback and no dual write.
- The old `storage.control_store`, `storage.sqlite`, and `storage.postgresql` formats are rejected.
  No compatibility parser, provider fallback, dual write, or data migration is retained.

## Architecture

```text
Control/Auth/ACL/RPC             MQTT protocol state
          |                              |
          v                              v
flowie_control_repository_t      flowie_protocol_repository_t
          |                              |
          +-----------+------------------+
                      v
              TurboDB ORM public ABI
                      |
          +-----------+------------+
          v                        v
   installed driver A       installed driver B
```

TurboDB gains a bounded materialized-result API for database operations whose output schema is
not known to a generic caller at compile time. The result owns copied cells until
`orm_result_destroy()`. Row count, column count, retained bytes, and parameter bytes remain
bounded by `orm_config_t`; overflow or capacity exhaustion fails before exposing a partial result.
The API is synchronous and single-owner. It does not share mutable result storage across threads.

Flowie then removes its PostgreSQL provider and its SQLite-shaped public store configuration.
The single Control repository receives an `orm_config_t`, owns one TurboDB connection, and
performs all commands, transactions, result access, and error handling through TurboDB. SQL used
for schema and operations is submitted only through TurboDB's public ABI; backend client types,
status constants, handles, and build targets never cross the TurboDB package boundary.

Portable raw statements use one-based `?N` parameters. TurboDB preserves them for SQLite and
normalizes them to `$N` for PostgreSQL without rewriting quoted strings, identifiers, comments, or
dollar-quoted bodies. TurboDB also owns backend error classification and per-connection invariants:
constraint violations have one structured status and every SQLite connection enables foreign-key
enforcement before use.

## LSQUIC Policy

Flowie does not consume the TurboNet LSQUIC target. Every Flowie-owned build of TurboNet,
including container and EU runbook builds, passes `TURBONET_ENABLE_LSQUIC=OFF`. A build helper
that cannot enforce this setting fails rather than silently accepting TurboNet defaults.

## Compatibility and Risk

- Public management RPC is unchanged.
- Control configuration is intentionally incompatible: only `storage.turbodb` is accepted.
  Secrets remain references and must be resolved before constructing a TurboDB option; literal
  database passwords are not accepted by a Flowie-specific field.
- Removing direct client-library targets changes the internal build graph. A TurboDB package
  without the configured driver must fail during provider creation.
- The highest risk is transaction/result parity. Shared repository contracts must run against
  each enabled TurboDB driver, followed by auth, ACL, management, and publish integration tests.

## Verification

- TurboDB result ABI tests cover text, blob, integer, boolean, null, command counts, capacity
  limits, overflow rejection, transaction ownership, and destruction after every failure point.
- Flowie repository contracts run through the TurboDB adapter; the optional live contract repeats
  the protocol repository test against PostgreSQL without linking or calling libpq from Flowie.
- Flowie config/runtime tests prove selection, validation, no fallback, and unchanged RPC
  behavior.
- The Flowie build graph contains no direct SQLite/PostgreSQL package or target.
- Flowie-owned TurboNet builds prove LSQUIC is disabled.
- Windows focused tests run first; the final PostgreSQL contract and container gate run on EU.
