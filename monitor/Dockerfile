ARG UBUNTU_IMAGE=192.168.10.35/xnet/ubuntu:22.04
FROM ${UBUNTU_IMAGE} AS python
ARG BUILD_CPUSET
ARG BUILD_JOBS=2
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        build-essential python3 xz-utils ca-certificates util-linux pkg-config \
        libssl-dev zlib1g-dev libbz2-dev libreadline-dev libsqlite3-dev libffi-dev \
        liblzma-dev libncursesw5-dev libgdbm-dev libgdbm-compat-dev libnsl-dev uuid-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /xcn
COPY docker/vendor.py docker/vendor.py
COPY docker/vendor/ docker/vendor/
COPY monitor/requirements.txt monitor/requirements.lock monitor/
RUN python3 docker/vendor.py verify monitor && python3 docker/vendor.py unpack python /build && \
    python3 docker/vendor.py unpack python-wheels /build
WORKDIR /build/Python-3.12.15
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    ./configure --prefix=/opt/python --enable-shared --enable-optimizations --with-lto \
        --without-static-libpython --with-ensurepip=install && \
    make -j "$BUILD_JOBS" && make install && \
    cp LICENSE /opt/python/LICENSE && \
    printf '%s\n' /opt/python/lib > /etc/ld.so.conf.d/xcn-python.conf && ldconfig && \
    /opt/python/bin/python3 -m pip install --no-index --find-links=/build/wheels \
        --require-hashes --no-cache-dir -r /xcn/monitor/requirements.lock && \
    /opt/python/bin/python3 -c 'import ssl,sqlite3,ctypes,bz2,lzma,readline,aiohttp,defusedxml' && \
    find /opt/python -type d -name __pycache__ -prune -exec rm -rf '{}' +

FROM ${UBUNTU_IMAGE} AS native
ARG BUILD_CPUSET
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends gcc libc6-dev util-linux && \
    rm -rf /var/lib/apt/lists/*
COPY lib/proto/ue-trace.c lib/proto/ue-trace.h /src/
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared /src/ue-trace.c -o /libxcn-trace.so

FROM ${UBUNTU_IMAGE} AS wireshark
ARG BUILD_CPUSET
ARG BUILD_JOBS=2
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        build-essential cmake ninja-build flex bison pkg-config xz-utils python3 util-linux \
        libglib2.0-dev libgcrypt20-dev libpcre2-dev zlib1g-dev libc-ares-dev libxml2-dev && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /xcn
COPY docker/vendor.py docker/vendor.py
COPY docker/vendor/ docker/vendor/
RUN python3 docker/vendor.py unpack wireshark /build
WORKDIR /build
RUN cmake -S wireshark-4.6.9 -B out -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_wireshark=OFF -DBUILD_stratoshark=OFF \
    -DBUILD_tshark=ON -DBUILD_dumpcap=OFF -DBUILD_sharkd=OFF -DENABLE_PCAP=OFF \
    -DENABLE_LUA=OFF -DENABLE_PLUGINS=OFF -DENABLE_GNUTLS=OFF \
    -DENABLE_KERBEROS=OFF -DENABLE_SMI=OFF \
    -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_BROTLI=OFF \
    -DENABLE_MINIZIP=OFF -DENABLE_SPEEXDSP=OFF -DENABLE_SPANDSP=OFF \
    -DENABLE_SBC=OFF -DENABLE_ILBC=OFF -DENABLE_NGHTTP2=OFF -DENABLE_NGHTTP3=OFF
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    cmake --build out --target tshark -j "$BUILD_JOBS" && mkdir /runtime && \
    cp -a out/run/tshark out/run/libwireshark.so* out/run/libwiretap.so* out/run/libwsutil.so* /runtime/ && \
    cp wireshark-4.6.9/COPYING /runtime/COPYING

FROM ${UBUNTU_IMAGE}
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        ca-certificates util-linux libstdc++6 libglib2.0-0 libgcrypt20 libpcre2-8-0 libc-ares2 libxml2 zlib1g \
        libssl3 libbz2-1.0 libreadline8 libsqlite3-0 libffi8 liblzma5 libncursesw6 \
        libgdbm6 libgdbm-compat4 libnsl2 libuuid1 && rm -rf /var/lib/apt/lists/*
COPY --from=python /opt/python/ /opt/python/
RUN printf '%s\n' /opt/python/lib > /etc/ld.so.conf.d/xcn-python.conf && ldconfig
COPY --from=wireshark /runtime/ /opt/wireshark/
ENV LD_LIBRARY_PATH=/opt/wireshark PATH=/opt/python/bin:/opt/wireshark:$PATH \
    PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1 PIP_NO_INDEX=1
WORKDIR /opt/xcn-monitor
COPY monitor/ .
COPY --from=native /libxcn-trace.so /opt/xcn-monitor/libxcn-trace.so
USER 65532:65532
# Agents and decoder use explicit commands; all four roles share this image.
ENTRYPOINT ["python3", "/opt/xcn-monitor/server.py"]
