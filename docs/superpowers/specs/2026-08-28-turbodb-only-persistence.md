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
- Backend names and backend connection options remain configuration data passed to TurboDB;
  they do not authorize Flowie to include or link a backend client library.
- Domain, role, group, user, credential, ACL, publish, revision, and audit semantics remain
  unchanged. Management RPC request and response contracts remain unchanged.
- There is one selected fact source. Connection or validation failure is fatal; there is no
  backend fallback and no dual write.
- Existing schemas are validated in place. This change does not delete user, credential, audit,
  or published-policy data.

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

Flowie then replaces its SQLite and libpq providers with one TurboDB adapter. SQL dialect and
schema lifecycle remain adapter concerns where the current ORM query builder cannot express a
recursive query or migration DDL, but execution is always performed by TurboDB. Backend client
types, status constants, and handles never cross the TurboDB package boundary.

## LSQUIC Policy

Flowie does not consume the TurboNet LSQUIC target. Every Flowie-owned build of TurboNet,
including container and EU runbook builds, passes `TURBONET_ENABLE_LSQUIC=OFF`. A build helper
that cannot enforce this setting fails rather than silently accepting TurboNet defaults.

## Compatibility and Risk

- Public management RPC is unchanged.
- Existing Control configuration remains accepted and is normalized into TurboDB driver/options
  at the composition boundary. No literal database password becomes accepted.
- Removing direct client-library targets changes the internal build graph. A TurboDB package
  without the configured driver must fail during provider creation.
- The highest risk is transaction/result parity. Shared repository contracts must run against
  each enabled TurboDB driver, followed by auth, ACL, management, and publish integration tests.

## Verification

- TurboDB result ABI tests cover text, blob, integer, boolean, null, command counts, capacity
  limits, overflow rejection, transaction ownership, and destruction after every failure point.
- Flowie repository contracts run through the TurboDB adapter.
- Flowie config/runtime tests prove selection, validation, no fallback, and unchanged RPC
  behavior.
- The Flowie build graph contains no direct SQLite/PostgreSQL package or target.
- Flowie-owned TurboNet builds prove LSQUIC is disabled.
- Windows focused tests run first; the final PostgreSQL contract and container gate run on EU.
