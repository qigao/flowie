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
ARG VCPKG_REF=b1b19307e2d2ec1eefbdb7ea069de7d4bcd31f01
ENV DEBIAN_FRONTEND=noninteractive \
    FLOWIE_BUILD_PRESET=${FLOWIE_BUILD_PRESET} \
    FLOWIE_INSTALL_PRESET=${FLOWIE_INSTALL_PRESET} \
    FLOWIE_PROFILE=${FLOWIE_PROFILE} \
    FLOWIE_ENABLE_ASAN=${FLOWIE_ENABLE_ASAN} \
    VCPKG_DISABLE_METRICS=1 \
    VCPKG_DEFAULT_BINARY_CACHE=/var/cache/vcpkg \
    VCPKG_ROOT=/opt/vcpkg

RUN apt-get update && apt-get install -y --no-install-recommends \
      autoconf bison build-essential ca-certificates curl flex git golang-go \
      libssl-dev libtool liburing-dev linux-libc-dev nasm ninja-build openssl \
      pkg-config python3 re2c tar unzip wget zip \
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
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

FROM build_toolchain AS turboutils_builder
COPY --from=turbo_utils . /src/turbo-utils
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/turbo-utils; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turboutils/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS turboparser_builder
COPY --from=turboutils_builder /opt/turboutils/ /opt/turboutils/
COPY --from=turbo_parser . /src/turbo-parser
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/turbo-parser; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turboparser/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DTURBO_ENABLE_CAPTURE=OFF \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"; \
    LD_LIBRARY_PATH="/opt/turboparser/$FLOWIE_PROFILE/lib:/opt/turboutils/$FLOWIE_PROFILE/lib" \
      ldd "/opt/turboparser/$FLOWIE_PROFILE/bin/tbe_compiler" \
      | tee "/opt/turboparser/$FLOWIE_PROFILE/tbe-compiler.ldd"; \
    ! grep -F 'not found' "/opt/turboparser/$FLOWIE_PROFILE/tbe-compiler.ldd"

FROM build_toolchain AS turbonet_builder
COPY --from=turboutils_builder /opt/turboutils/ /opt/turboutils/
COPY --from=turboparser_builder /opt/turboparser/ /opt/turboparser/
COPY --from=turbo_net . /src/turbonet
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/turbonet; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turbonet/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DTURBONET_ENABLE_LSQUIC=OFF \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

# Opt-in CoroNet logging regression stage. Building this target exercises the
# Linux epoll backend without adding test binaries to the Flowie runtime image.
FROM turbonet_builder AS turbonet_logging_tests
RUN set -eux; cd /src/turbonet; \
    cmake --build --preset "$FLOWIE_BUILD_PRESET" --parallel "$(nproc)" \
      --target test_coronet_logging test_coronet_epoll_fault_logging; \
    ctest --preset "$FLOWIE_BUILD_PRESET" --output-on-failure \
      -R '^test_coronet_(logging|epoll_fault_logging)$'

FROM build_toolchain AS turbodb_builder
COPY --from=turboutils_builder /opt/turboutils/ /opt/turboutils/
COPY --from=turboparser_builder /opt/turboparser/ /opt/turboparser/
COPY --from=turbonet_builder /opt/turbonet/ /opt/turbonet/
COPY --from=turbo_db . /src/turbodb
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/turbodb; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turbodb/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DORM_WITH_PGSQL=ON -DORM_BUILD_SHARED=ON \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS turbohttp_builder
COPY --from=turboutils_builder /opt/turboutils/ /opt/turboutils/
COPY --from=turboparser_builder /opt/turboparser/ /opt/turboparser/
COPY --from=turbonet_builder /opt/turbonet/ /opt/turbonet/
COPY --from=turbodb_builder /opt/turbodb/ /opt/turbodb/
COPY --from=turbo_http . /src/TurboHTTP
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/TurboHTTP; \
    cmake --fresh --preset "$FLOWIE_BUILD_PRESET" \
      -DCMAKE_INSTALL_PREFIX="/opt/turbohttp/$FLOWIE_PROFILE" \
      -DENABLE_SANITIZER_ADDRESS="$FLOWIE_ENABLE_ASAN" \
      -DBUILD_TESTING=OFF -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF; \
    cmake --build --preset "$FLOWIE_INSTALL_PRESET" --parallel "$parallel"

FROM build_toolchain AS builder
ARG SOURCE_REVISION=unknown
COPY --from=turboutils_builder /opt/turboutils/ /opt/turboutils/
COPY --from=turboparser_builder /opt/turboparser/ /opt/turboparser/
COPY --from=turbonet_builder /opt/turbonet/ /opt/turbonet/
COPY --from=turbodb_builder /opt/turbodb/ /opt/turbodb/
COPY --from=turbohttp_builder /opt/turbohttp/ /opt/turbohttp/
COPY --from=turboutils_builder /src/turbo-utils/vcpkg_installed/ /src/turbo-utils/vcpkg_installed/
COPY --from=turboparser_builder /src/turbo-parser/vcpkg_installed/ /src/turbo-parser/vcpkg_installed/
COPY --from=turbonet_builder /src/turbonet/vcpkg_installed/ /src/turbonet/vcpkg_installed/
COPY --from=turbodb_builder /src/turbodb/vcpkg_installed/ /src/turbodb/vcpkg_installed/
COPY --from=turbohttp_builder /src/TurboHTTP/vcpkg_installed/ /src/TurboHTTP/vcpkg_installed/
COPY . /src/flowie
RUN --mount=type=cache,target=/var/cache/vcpkg,sharing=locked \
    set -eux; parallel="$(nproc)"; cd /src/flowie; \
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
    for libdir in \
      "/opt/turboutils/$FLOWIE_PROFILE/lib" "/opt/turboparser/$FLOWIE_PROFILE/lib" \
      "/opt/turbonet/$FLOWIE_PROFILE/lib" "/opt/turbodb/$FLOWIE_PROFILE/lib" \
      "/opt/turbohttp/$FLOWIE_PROFILE/lib" "/opt/flowie/$FLOWIE_PROFILE/lib" \
      /src/turbo-utils/vcpkg_installed/x64-linux/debug/lib \
      /src/turbo-utils/vcpkg_installed/x64-linux/lib \
      /src/turbo-parser/vcpkg_installed/x64-linux/debug/lib \
      /src/turbo-parser/vcpkg_installed/x64-linux/lib \
      /src/turbonet/vcpkg_installed/x64-linux/debug/lib \
      /src/turbonet/vcpkg_installed/x64-linux/lib \
      /src/turbodb/vcpkg_installed/x64-linux/debug/lib \
      /src/turbodb/vcpkg_installed/x64-linux/lib \
      /src/TurboHTTP/vcpkg_installed/x64-linux/debug/lib \
      /src/TurboHTTP/vcpkg_installed/x64-linux/lib \
      /src/flowie/vcpkg_installed/x64-linux/debug/lib \
      /src/flowie/vcpkg_installed/x64-linux/lib; do \
      if [ -d "$libdir" ]; then \
        find "$libdir" -maxdepth 1 \( -type f -o -type l \) -name '*.so*' \
          -exec cp -a '{}' /opt/flowie-runtime/lib/ ';'; \
      fi; \
    done; \
    for executable in flowie_server flowie-control flowie-control-data; do \
      case "$executable" in \
        flowie_server) manifest=flowie-server ;; \
        *) manifest=$executable ;; \
      esac; \
      LD_LIBRARY_PATH=/opt/flowie-runtime/lib ldd "/opt/flowie-runtime/bin/$executable" \
        | tee "/opt/flowie-runtime/$manifest.ldd"; \
      ! grep -F 'not found' "/opt/flowie-runtime/$manifest.ldd"; \
    done

FROM debian:bookworm-slim AS runtime
ARG SOURCE_REVISION=unknown
LABEL org.opencontainers.image.title="Flowie server and control" \
      org.opencontainers.image.description="Flowie MQTT broker and HTTPS control runtime" \
      org.opencontainers.image.revision="${SOURCE_REVISION}"

RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libgcc-s1 libstdc++6 netcat-openbsd \
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

RUN ldconfig \
    && for executable in flowie_server flowie-control flowie-control-data; do \
         case "$executable" in \
           flowie_server) manifest=flowie-server ;; \
           *) manifest=$executable ;; \
         esac; \
         ldd "/usr/local/bin/$executable" \
           | tee "/usr/share/doc/flowie/$manifest-runtime.ldd"; \
         ! grep -F 'not found' "/usr/share/doc/flowie/$manifest-runtime.ldd"; \
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
