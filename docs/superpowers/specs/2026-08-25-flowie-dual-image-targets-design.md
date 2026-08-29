# Flowie Dual Image Targets Design

## Status

Approved direction: one Flowie source revision produces two explicit Docker image targets. The
standalone target preserves the current MQTT-only product contract. The Control target restores
the PicImpact production contract in which one `flowie_server` process owns the configured MQTT
runtime and the embedded Flowie Control runtime.

## Goal

Publish and verify two immutable images from the same Flowie and dependency revisions:

- `flowie-standalone`: the existing listener-option-driven MQTT broker used for isolated broker,
  load, and transport tests;
- `flowie-control`: the configured MQTT application plus embedded Control used by PicImpact.

The two images must not depend on long-lived Git branches or copy source trees. Their different
runtime contracts are expressed as named Docker build targets, separate entrypoints, and separate
tests in one repository.

## Current State and Root Cause

The current `flowie_server` executable is a standalone MQTT broker. It accepts listener and
capacity flags such as `--host`, `--port`, and `--max-connections`. The current server image and
entrypoint correctly implement that standalone contract.

The configured server application, Control runtime, YAML/Graph loading, and integration tests
remain in the source tree, but the configured application target is no longer wired into the
current server executable or CMake graph. As a result, mounting `/etc/flowie/control.yml` or adding
`--control-config` to the current image cannot enable Control: the executable does not accept the
configured-server CLI and never constructs `flowie_server_application_t`.

This mismatch explains the EU failure. The PicImpact Compose contract mounts the broker, graph,
and Control configuration and expects listeners on `127.0.0.1:18883` and `127.0.0.1:8443`, while
the deployed image is explicitly labelled and started as a standalone MQTT broker.

## Alternatives

### Named image targets with distinct executables (selected)

Keep the current standalone executable unchanged. Add a separately named configured executable
that links the existing server application and Control runtime. The standalone image copies the
standalone executable; the Control image copies the configured executable to the runtime name
`/usr/local/bin/flowie_server` and uses the established configured entrypoint.

This preserves the current standalone CLI, restores PicImpact behavior, makes image intent visible
at build time, and prevents one entrypoint from guessing a runtime mode.

### One executable with two CLI modes

Merge standalone and configured argument parsing into one executable. This keeps one installed
binary but creates ambiguous option combinations, increases regression risk, and makes the
security-sensitive production path share a broad parser with load-test tuning. It is rejected.

### Two processes in one container

Start the current standalone broker and an independent `flowie-control` process under a shell
supervisor. This puts both listeners in Docker but does not restore the previous MQTT Auth/ACL
composition: the standalone broker does not consume the configured HTTPS Auth/ACL providers.
It is rejected because a healthy Control port would conceal an unauthenticated data plane.

## Build Architecture

The Dockerfile exposes two final targets:

```text
shared dependency and Flowie builders
              |
              +--> runtime-standalone --> flowie-standalone image
              |
              `--> runtime-control ----> flowie-control image
```

Both targets record the exact Flowie and dependency revisions in OCI labels. Both use the same
non-root UID/GID, read-only root filesystem contract, shared-library closure validation, init,
healthcheck conventions, and base operating-system packages.

The standalone runtime contains the existing `flowie_server` and standalone entrypoint. The
Control runtime contains the configured server executable, installed as `flowie_server`, plus the
configured entrypoint, dashboard resources, and all shared libraries required by the Control,
HTTP, YAML/Graph, and persistence paths.

The configured executable is enabled by an explicit CMake option. Normal standalone builds do not
find or link the configured application dependencies. A Control-image build fails during CMake
configuration if any required application, Control, HTTP, database, or graph target is unavailable.
There is no runtime fallback to standalone mode.

## Runtime Contracts

### `flowie-standalone`

The image preserves the current environment-to-CLI mapping and default listener behavior. It does
not mount or read `flowie.yml`, `flowie.flow`, `control.yml`, database credentials, or Control TLS
keys. Its OCI description states that it is a standalone MQTT broker.

### `flowie-control`

The image requires these readable files before starting:

- `/etc/flowie/flowie.yml`;
- `/etc/flowie/flowie.flow`;
- `/etc/flowie/control.yml`.

Its entrypoint invokes the configured executable with `--require-security`, `--profile`,
`--control-config`, and the YAML/Graph positional inputs. It accepts storage plugin paths through
the existing bounded, validated list. Missing files, unsupported protocol-store settings, invalid
security composition, database failure, migration failure, certificate failure, or an unavailable
Control listener all fail startup. The image never silently starts an unsecured standalone broker.

One process owns both runtimes. Startup order is Control creation/start followed by MQTT worker
start. If MQTT startup fails, the application stops and destroys Control before exiting. Shutdown
stops the MQTT worker before Control, then releases application resources. PostgreSQL remains the
Control business-state fact source; MQTT protocol state remains a separate process-lifetime fact
source under the currently supported standalone protocol-store contract.

## PicImpact Deployment Contract

`docker-compose.flowie.yml` selects only the `flowie-control` image. It continues to mount the
three configuration files, certificates, optional plugins, and Flowie state, and it supplies the
Control PostgreSQL password and service credentials through environment-backed secret references.
The standalone image is never a valid value for the PicImpact Flowie infrastructure service.

Deployment tooling verifies the image flavor before replacement by checking an OCI flavor label
and the configured executable's `--check` result. This prevents a healthy MQTT-only image from
being promoted into the PicImpact slot again.

## EU Dev Data Rebuild

The requested clean reconfiguration applies only to database/configuration state on the EU dev
environment. Before any destructive operation, the deployment creates a timestamped root-only cold
backup and records exact source paths and database identities.

The reset scope is:

- Flowie Control PostgreSQL database/schema and its users, credentials, roles, groups, policies,
  revisions, and audit records;
- PicImpact PostgreSQL business schema;
- PicImpact SQLite configuration database.

The reset explicitly preserves uploads, Flowie YAML/Graph files, TLS certificates and private keys,
Let's Encrypt state, PostgreSQL TLS material, object-storage data, and RustFS data. Destructive
commands operate only on resolved, named EU dev targets after all writers are stopped. Recovery is
the inverse cold operation: stop the new writers and restore the timestamped database/configuration
backup.

After reset, initialization order is PostgreSQL boundary creation, Flowie configured-server
validation and bootstrap, PicImpact schema initialization, Flowie domain/admin/service credential
provisioning, and finally PicImpact-derived tenant/station/printer synchronization.

## Verification

Verification proceeds from the smallest contract to the deployed system:

1. Standalone entrypoint regression test proves the existing listener/capacity argument vector.
2. Control entrypoint regression test proves required-file checks and configured argument vector.
3. Configured executable tests prove `--check`, required security, invalid Control config rejection,
   and lifecycle cleanup.
4. A local Control image starts both listeners, passes Control HTTPS readiness, and completes MQTT
   authentication, ACL allow, and ACL deny cases.
5. A local standalone image starts only MQTT and has no listener on the Control port.
6. EU preflight verifies immutable image identity, flavor label, configuration, certificate paths,
   backup availability, and rollback image.
7. EU deployment verifies container health, exactly one configured Flowie process, listeners on
   `127.0.0.1:18883` and `127.0.0.1:8443`, Control management/auth/ACL endpoints, PicImpact backend
   service-account synchronization, MQTT connect/publish/subscribe, expected ACL denial, and clean
   restart without unexpected native Flowie services.

A TCP-only healthcheck is insufficient for the Control image. Its healthcheck must require both the
MQTT listener and a non-mutating Control readiness endpoint over the configured TLS trust chain.

## Rollback

The existing immutable Flowie and PicImpact image references are recorded before deployment. Image
rollback replaces containers without modifying the cold backup. Data rollback is performed only if
the newly initialized state must be abandoned: stop all writers, archive the failed new state, and
restore the cold backup as one consistent generation.

Rollback never starts the standalone image in the PicImpact slot and never restores only one member
of the PostgreSQL/PicImpact configuration generation.

## Security and Compatibility

The standalone public CLI and its tests remain compatible. The Control image restores the previous
PicImpact deployment behavior and keeps security fail-closed. Secrets are not copied into image
layers, printed by build commands, included in OCI labels, or written to verification artifacts.

The build adds no implicit provider fallback and no shared mutable state between image flavors.
Configuration format, Control HTTP contracts, MQTT protocol behavior, and PicImpact Compose mounts
remain unchanged. The only deployment-visible addition is an explicit image-flavor identity used
as a promotion gate.
