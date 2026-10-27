ARG UBUNTU_IMAGE=192.168.10.35/xnet/ubuntu:22.04
FROM ${UBUNTU_IMAGE} AS build

ARG BUILD_CPUSET
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        ca-certificates xz-utils python3 build-essential git util-linux && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /xcn
COPY docker/vendor.py docker/vendor.py
COPY docker/vendor/ docker/vendor/
COPY webui/package.json webui/package-lock.json webui/
RUN python3 docker/vendor.py verify webui && \
    python3 docker/vendor.py unpack node /opt && \
    mv /opt/node-v19.9.0-linux-x64 /opt/node && \
    python3 docker/vendor.py unpack npm-cache /tmp && \
    cp docker/vendor/archives/wait-2.9.0 /wait && chmod +x /wait
ENV PATH=/opt/node/bin:$PATH NEXT_TELEMETRY_DISABLED=1
WORKDIR /usr/src/xcn/webui
COPY webui/package.json webui/package-lock.json ./
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    npm ci --offline --no-audit --fund=false --cache /tmp/npm-cache
COPY webui/ ./
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    npm run build && \
    npm prune --omit=dev --ignore-scripts --offline --no-audit --fund=false --cache /tmp/npm-cache

FROM ${UBUNTU_IMAGE}
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
# Preserve operational commands; compilers, caches and source archives stay in build.
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        ca-certificates curl wget binutils file procps util-linux python3 \
        openssl xz-utils bzip2 netbase libstdc++6 && \
    rm -rf /var/lib/apt/lists/*
ENV PATH=/opt/node/bin:$PATH NEXT_TELEMETRY_DISABLED=1
WORKDIR /usr/src/xcn/webui
COPY --from=build /opt/node/ /opt/node/
COPY --from=build /usr/src/xcn/webui/ ./
COPY --from=build /wait /wait
COPY --from=build /xcn/docker/vendor/archives/wait-2.9.0.LICENSE /usr/share/doc/xcn-wait/LICENSE
CMD /wait && npm run start
EXPOSE 9999
