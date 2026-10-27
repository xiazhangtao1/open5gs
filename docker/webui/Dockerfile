FROM node:19.9.0-bullseye AS build

ARG BUILD_CPUSET
ARG WAIT_SHA256=b945d1bc2b6bac5b8998bdc56791a66d99085432a6b164ef6726c52653f4b8d5

WORKDIR /usr/src/xcn/webui
COPY webui/package.json webui/package-lock.json ./
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    npm ci
COPY webui/ ./
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    npm run build && npm prune --omit=dev --ignore-scripts
RUN curl -fsSL --retry 3 \
        https://github.com/ufoscout/docker-compose-wait/releases/download/2.9.0/wait -o /wait && \
    printf '%s  /wait\n' "$WAIT_SHA256" | sha256sum -c - && chmod +x /wait

FROM node:19.9.0-bullseye-slim

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
# Preserve operational commands; build dependencies and caches stay in build.
# Bullseye security packages moved to Debian's signed archive after LTS ended.
RUN sed -i 's@deb.debian.org/debian-security@archive.debian.org/debian-security@g' /etc/apt/sources.list && \
    apt-get -o Acquire::Retries=3 update && apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        ca-certificates curl wget binutils file procps util-linux python3 \
        openssl xz-utils bzip2 netbase && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /usr/src/xcn/webui
COPY --from=build /usr/src/xcn/webui/ ./
COPY --from=build /wait /wait

CMD /wait && npm run start
EXPOSE 9999
