# syntax=docker/dockerfile:1.7

ARG FLOWIE_BUILD_PRESET=linux-release-user
ARG FLOWIE_INSTALL_PRESET=install-linux-release-user
ARG FLOWIE_PROFILE=release
ARG FLOWIE_ENABLE_ASAN=OFF

FROM debian:bookworm AS build_toolchain

ARG FLOWIE_BUILD_PRESET
ARG FLOWIE_INSTALL_PRESET
ARG FLOWIE_PROFILE
ARG FLOWIE_ENABLE_ASAN
ARG CMAKE_VERSION=3.31.0
ARG CMAKE_SHA256=7cfdf4a411c71d13c027199952fd25e8245d85c932ff452e2b9a9e0f6dfe368a
ARG RE2C_VERSION=4.6
ARG RE2C_SHA256=75bf2696445e831d0d44e0d9f2909eeffc18c09757f222b2fb025f7e59fe130b
ARG VCPKG_REF=b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01
ARG VCPKG_CACHE_REF=0dc254695917bc180504d916240292e9096e9da5
ENV DEBIAN_FRONTEND=noninteractive \
    FLOWIE_BUILD_PRESET=${FLOWIE_BUILD_PRESET} \
    FLOWIE_INSTALL_PRESET=${FLOWIE_INSTALL_PRESET} \
    FLOWIE_PROFILE=${FLOWIE_PROFILE} \
    FLOWIE_ENABLE_ASAN=${FLOWIE_ENABLE_ASAN} \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg \
    VCPKG_ROOT=/opt/vcpkg \
    PROJECT_ROOT=/opt/flowie-build \
    RE2C_ROOT=/opt/re2c

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf bison build-essential ca-certificates curl flex git golang-go \
      libtool liburing-dev linux-libc-dev nasm ninja-build \
      mono-complete pkg-config python3 tar unzip wget xz-utils zip \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
      -o /tmp/cmake-install.sh \
    && echo "${CMAKE_SHA256}  /tmp/cmake-install.sh" | sha256sum -c - \
    && chmod +x /tmp/cmake-install.sh \
    && /tmp/cmake-install.sh --skip-license --prefix=/usr/local \
    && rm -f /tmp/cmake-install.sh \
    && git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout "${VCPKG_REF}" \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && git clone https://github.com/qigao/vcpkg-cache /root/.cache/qigao/vcpkg-cache \
    && git -C /root/.cache/qigao/vcpkg-cache checkout --detach "${VCPKG_CACHE_REF}"

WORKDIR /tmp/re2c-source
RUN curl -fsSL "https://github.com/skvadrik/re2c/releases/download/${RE2C_VERSION}/re2c-${RE2C_VERSION}.tar.xz" \
      -o /tmp/re2c.tar.xz \
    && echo "${RE2C_SHA256}  /tmp/re2c.tar.xz" | sha256sum -c - \
    && tar -xf /tmp/re2c.tar.xz --strip-components=1 \
    && ./configure --prefix="$RE2C_ROOT" --disable-dependency-tracking \
    && make -j"$(nproc)" \
    && make install \
    && rm /tmp/re2c.tar.xz

# Reject mixed profile arguments before any SDK is configured.
RUN set -eu; \
    case "$FLOWIE_PROFILE:$FLOWIE_BUILD_PRESET:$FLOWIE_INSTALL_PRESET" in \
      release:linux-release-user:install-linux-release-user|debug:linux-dev-user:install-linux-dev-user) ;; \
      *) echo 'FLOWIE profile and presets must select the same Debug/Release profile' >&2; exit 2 ;; \
    esac

FROM build_toolchain AS salts_builder
COPY --from=salts . /src/salts
WORKDIR /src/salts
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    --mount=type=secret,id=github_token,required=true \
    set -eu; GITHUB_TOKEN="$(cat /run/secrets/github_token)"; export GITHUB_TOKEN; \
    : "${GITHUB_TOKEN:?github_token must not be empty}"; parallel="$(nproc)"; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/salts/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS salts_utils_builder
COPY --from=salts_builder /opt/salts/ /opt/salts/
COPY --from=salts_utils . /src/salts-utils
COPY --from=salts_builder /opt/salts/${FLOWIE_PROFILE}/ /src/salts-utils/stage/dependencies/salts/linux-x64/
WORKDIR /src/salts-utils
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    --mount=type=secret,id=github_token,required=true \
    set -eu; GITHUB_TOKEN="$(cat /run/secrets/github_token)"; export GITHUB_TOKEN; \
    : "${GITHUB_TOKEN:?github_token must not be empty}"; parallel="$(nproc)"; \
    export SALTS_ROOT="/opt/salts/$FLOWIE_PROFILE"; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/salts-utils/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DSALTS_UTILS_ENABLE_CAPTURE=OFF \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"; \
    LD_LIBRARY_PATH="/opt/salts-utils/$FLOWIE_PROFILE/lib:/opt/salts/$FLOWIE_PROFILE/lib" \
      ldd "/opt/salts-utils/$FLOWIE_PROFILE/bin/salts-idlc" \
      > "/opt/salts-utils/$FLOWIE_PROFILE/salts-idlc.ldd"; \
    cat "/opt/salts-utils/$FLOWIE_PROFILE/salts-idlc.ldd"; \
    if grep -Fq 'not found' "/opt/salts-utils/$FLOWIE_PROFILE/salts-idlc.ldd"; then exit 1; fi

FROM build_toolchain AS chttp_builder
COPY --from=salts_builder /opt/salts/ /opt/salts/
COPY --from=salts_utils_builder /opt/salts-utils/ /opt/salts-utils/
COPY --from=chttp . /src/chttp
WORKDIR /src/chttp
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    --mount=type=secret,id=github_token,required=true \
    set -eu; GITHUB_TOKEN="$(cat /run/secrets/github_token)"; export GITHUB_TOKEN; \
    : "${GITHUB_TOKEN:?github_token must not be empty}"; parallel="$(nproc)"; \
    export SALTS_ROOT="/opt/salts/$FLOWIE_PROFILE"; \
    export SALTS_UTILS_ROOT="/opt/salts-utils/$FLOWIE_PROFILE"; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/http-services/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS turbodb_builder
COPY --from=salts_builder /opt/salts/ /opt/salts/
COPY --from=salts_utils_builder /opt/salts-utils/ /opt/salts-utils/
COPY --from=turbo_db . /src/turbodb
COPY --from=salts_builder /opt/salts/${FLOWIE_PROFILE}/ /src/turbodb/stage/dependencies/salts/linux-x64/
COPY --from=salts_utils_builder /opt/salts-utils/${FLOWIE_PROFILE}/ /src/turbodb/stage/dependencies/salts-utils/linux-x64/
WORKDIR /src/turbodb
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    --mount=type=secret,id=github_token,required=true \
    set -eu; GITHUB_TOKEN="$(cat /run/secrets/github_token)"; export GITHUB_TOKEN; \
    : "${GITHUB_TOKEN:?github_token must not be empty}"; parallel="$(nproc)"; \
    export SALTS_ROOT="/opt/salts/$FLOWIE_PROFILE"; \
    export SALTS_UTILS_ROOT="/opt/salts-utils/$FLOWIE_PROFILE"; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turbodb/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DTURBODB_BUILD_ORM=ON -DTURBODB_BUILD_TIDESSQL=OFF \
      -DTURBODB_BUILD_REDIS=OFF -DTURBODB_BUILD_DBTOOLS=OFF \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS builder
ARG SOURCE_REVISION=unknown
COPY --from=salts_builder /opt/salts/ /opt/salts/
COPY --from=chttp_builder /opt/http-services/ /opt/http-services/
COPY --from=salts_utils_builder /opt/salts-utils/ /opt/salts-utils/
COPY --from=turbodb_builder /opt/turbodb/ /opt/turbodb/
COPY --from=salts_builder /src/salts/vcpkg_installed/ /src/salts/vcpkg_installed/
COPY --from=chttp_builder /src/chttp/vcpkg_installed/ /src/chttp/vcpkg_installed/
COPY --from=salts_utils_builder /src/salts-utils/vcpkg_installed/ /src/salts-utils/vcpkg_installed/
COPY --from=turbodb_builder /src/turbodb/vcpkg_installed/ /src/turbodb/vcpkg_installed/
COPY . /src/flowie
WORKDIR /src/flowie
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    --mount=type=secret,id=github_token,required=true \
    set -eu; GITHUB_TOKEN="$(cat /run/secrets/github_token)"; export GITHUB_TOKEN; \
    : "${GITHUB_TOKEN:?github_token must not be empty}"; parallel="$(nproc)"; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/flowie/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DFLOWIE_BUILD_CLUSTER=OFF \
      -DFLOWIE_BUILD_TESTS=OFF \
      -DBUILD_TESTING=OFF -DENABLE_TESTS=OFF -DBUILD_TESTS=OFF \
      -DBUILD_EXAMPLES=OFF \
      -DFLOWIE_CONTROL_DASHBOARD_RESOURCE_DIR=/opt/flowie/share/control/dashboard-resources \
      -DFLOWIE_RELEASE_REVISION="${SOURCE_REVISION}"; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"; \
    install -d /opt/flowie-runtime/bin /opt/flowie-runtime/lib; \
    install -m 0755 "/opt/flowie/$FLOWIE_PROFILE/bin/flowie_server" \
      /opt/flowie-runtime/bin/flowie_server; \
    install -m 0755 "/opt/flowie/$FLOWIE_PROFILE/bin/flowie-control" \
      /opt/flowie-runtime/bin/flowie-control; \
    install -m 0755 "/opt/flowie/$FLOWIE_PROFILE/bin/flowie-control-data" \
      /opt/flowie-runtime/bin/flowie-control-data; \
    case "$FLOWIE_PROFILE" in \
      debug) vcpkg_libdir=debug/lib ;; \
      release) vcpkg_libdir=lib ;; \
    esac; \
    for libdir in \
      "/opt/salts/$FLOWIE_PROFILE/lib" "/opt/http-services/$FLOWIE_PROFILE/lib" \
      "/opt/salts-utils/$FLOWIE_PROFILE/lib" \
      "/opt/turbodb/$FLOWIE_PROFILE/lib" \
      "/opt/flowie/$FLOWIE_PROFILE/lib" \
      "/src/salts/vcpkg_installed/x64-linux/$vcpkg_libdir" \
      "/src/chttp/vcpkg_installed/x64-linux/$vcpkg_libdir" \
      "/src/salts-utils/vcpkg_installed/x64-linux/$vcpkg_libdir" \
      "/src/turbodb/vcpkg_installed/x64-linux/$vcpkg_libdir" \
      "/src/flowie/vcpkg_installed/x64-linux/$vcpkg_libdir"; do \
      if [ -d "$libdir" ]; then \
        find "$libdir" -maxdepth 1 \( -type f -o -type l \) -name '*.so*' \
          -exec cp -a '{}' /opt/flowie-runtime/lib/ ';'; \
      fi; \
    done; \
    driver_source="/opt/turbodb/$FLOWIE_PROFILE/lib/turbodb/drivers"; \
    driver_stage="/opt/flowie-runtime/turbodb/$FLOWIE_PROFILE/lib/turbodb/drivers"; \
    test -f "$driver_source/turbodb_driver_sqlite.so"; \
    install -d "$driver_stage"; \
    for module in "$driver_source"/*.so; do \
      install -m 0755 "$module" "$driver_stage/"; \
      LD_LIBRARY_PATH=/opt/flowie-runtime/lib ldd "$module" \
        > /opt/flowie-runtime/driver.ldd; \
      cat /opt/flowie-runtime/driver.ldd; \
      if grep -Fq 'not found' /opt/flowie-runtime/driver.ldd; then exit 1; fi; \
    done; \
    for executable in flowie_server flowie-control flowie-control-data; do \
      case "$executable" in \
        flowie_server) manifest=flowie-server ;; \
        *) manifest=$executable ;; \
      esac; \
      LD_LIBRARY_PATH=/opt/flowie-runtime/lib ldd "/opt/flowie-runtime/bin/$executable" \
        > "/opt/flowie-runtime/$manifest.ldd"; \
      cat "/opt/flowie-runtime/$manifest.ldd"; \
      if grep -Fq 'not found' "/opt/flowie-runtime/$manifest.ldd"; then exit 1; fi; \
    done

FROM debian:bookworm-slim AS runtime
ARG SOURCE_REVISION=unknown
ARG FLOWIE_ENABLE_ASAN
LABEL org.opencontainers.image.title="Flowie server and control" \
      org.opencontainers.image.description="Flowie MQTT broker and HTTPS control runtime" \
      org.opencontainers.image.revision="${SOURCE_REVISION}"

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgcc-s1 libstdc++6 netcat-openbsd \
    && case "$FLOWIE_ENABLE_ASAN" in \
         ON) apt-get install -y --no-install-recommends libasan8 ;; \
         OFF) ;; \
         *) echo 'FLOWIE_ENABLE_ASAN must be ON or OFF' >&2; exit 2 ;; \
       esac \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 10001 flowie \
    && useradd --uid 10001 --gid flowie --no-create-home --home-dir /var/lib/flowie \
         --shell /usr/sbin/nologin flowie \
    && install -d -o flowie -g flowie \
         /etc/flowie /etc/flowie/certs /opt/flowie/plugins /var/lib/flowie/control

COPY --from=builder /opt/flowie-runtime/bin/flowie_server /usr/local/bin/flowie_server
COPY --from=builder /opt/flowie-runtime/bin/flowie-control /usr/local/bin/flowie-control
COPY --from=builder /opt/flowie-runtime/bin/flowie-control-data /usr/local/bin/flowie-control-data
COPY --from=builder /opt/flowie-runtime/lib/ /usr/local/lib/
# Preserve the absolute SDK driver paths embedded by Flowie's configure step.
COPY --from=builder /opt/flowie-runtime/turbodb/ /opt/turbodb/
COPY --from=builder /opt/flowie/share/ /opt/flowie/share/
COPY --from=builder /opt/flowie-runtime/flowie-server.ldd /usr/share/doc/flowie/flowie-server.ldd
COPY --from=builder /opt/flowie-runtime/flowie-control.ldd /usr/share/doc/flowie/flowie-control.ldd
COPY --from=builder /opt/flowie-runtime/flowie-control-data.ldd /usr/share/doc/flowie/flowie-control-data.ldd
COPY --chmod=0755 deploy/server/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh
COPY --chmod=0755 deploy/server/flowie-control-entrypoint.sh \
  /usr/local/bin/flowie-control-entrypoint
COPY --chmod=0755 deploy/server/flowie-combined-entrypoint.sh \
  /usr/local/bin/flowie-combined-entrypoint
COPY --chmod=0755 deploy/server/healthcheck.sh /usr/local/bin/flowie-healthcheck

RUN set -eu; ldconfig; \
    for executable in flowie_server flowie-control flowie-control-data; do \
         case "$executable" in \
           flowie_server) manifest=flowie-server ;; \
           *) manifest=$executable ;; \
         esac; \
         ldd "/usr/local/bin/$executable" \
           > "/usr/share/doc/flowie/$manifest-runtime.ldd"; \
         cat "/usr/share/doc/flowie/$manifest-runtime.ldd"; \
         if grep -Fq 'not found' "/usr/share/doc/flowie/$manifest-runtime.ldd"; then exit 1; fi; \
       done

ENV FLOWIE_HOST=127.0.0.1 \
    FLOWIE_PORT=18883 \
    FLOWIE_TRANSPORT=tcp \
    FLOWIE_PATH=/mqtt \
    FLOWIE_PROTOCOL_STORE_DRIVER=sqlite \
    FLOWIE_PROTOCOL_STORE_OPTIONS='{"filename":"/var/lib/flowie/flowie-protocol.sqlite3"}' \
    FLOWIE_MAX_CONNECTIONS=1024 \
    FLOWIE_MAX_INFLIGHT_PER_SESSION=64 \
    FLOWIE_LOG_LEVEL=INFO \
    FLOWIE_HEALTH_HOST=127.0.0.1 \
    FLOWIE_HEALTH_PORT=18883

VOLUME ["/var/lib/flowie"]
WORKDIR /var/lib/flowie
USER 10001:10001
HEALTHCHECK --interval=30s --timeout=5s --start-period=15s --retries=3 \
  CMD ["/usr/local/bin/flowie-healthcheck"]
ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
