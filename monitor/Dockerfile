ARG PYTHON_IMAGE=python:3.12-slim
FROM ${PYTHON_IMAGE} AS native
ARG BUILD_CPUSET
RUN apt-get update && apt-get install -y --no-install-recommends gcc libc6-dev && rm -rf /var/lib/apt/lists/*
COPY lib/proto/ue-trace.c lib/proto/ue-trace.h /src/
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    cc -std=gnu11 -O2 -Wall -Wextra -Werror -fPIC -shared /src/ue-trace.c -o /libxcn-trace.so


FROM ${PYTHON_IMAGE} AS wireshark
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build flex bison pkg-config curl xz-utils \
    libglib2.0-dev libgcrypt20-dev libpcre2-dev zlib1g-dev libc-ares-dev libxml2-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN curl -fsSL --retry 3 https://www.wireshark.org/download/src/wireshark-4.6.9.tar.xz -o source.tar.xz \
    && echo 'd062d416a1ab3a49975aafe3a671c882dde0562ccd71b078808ceb59d465fc0f  source.tar.xz' | sha256sum -c - \
    && tar -xf source.tar.xz && rm source.tar.xz
RUN cmake -S wireshark-4.6.9 -B out -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_wireshark=OFF -DBUILD_stratoshark=OFF \
    -DBUILD_tshark=ON -DBUILD_dumpcap=OFF -DBUILD_sharkd=OFF -DENABLE_PCAP=OFF \
    -DENABLE_LUA=OFF -DENABLE_PLUGINS=OFF -DENABLE_GNUTLS=OFF \
    -DENABLE_KERBEROS=OFF -DENABLE_SMI=OFF \
    -DENABLE_LZ4=OFF -DENABLE_ZSTD=OFF -DENABLE_BROTLI=OFF \
    -DENABLE_MINIZIP=OFF -DENABLE_SPEEXDSP=OFF -DENABLE_SPANDSP=OFF \
    -DENABLE_SBC=OFF -DENABLE_ILBC=OFF -DENABLE_NGHTTP2=OFF -DENABLE_NGHTTP3=OFF
# Keep builds on explicitly supplied spare CPUs too; image builds share the host.
ARG BUILD_CPUSET
ARG BUILD_JOBS=2
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    cmake --build out --target tshark -j "$BUILD_JOBS" \
    && mkdir /runtime \
    && cp -a out/run/tshark out/run/libwireshark.so* out/run/libwiretap.so* out/run/libwsutil.so* /runtime/


FROM ${PYTHON_IMAGE}
RUN apt-get update && apt-get install -y --no-install-recommends libglib2.0-0 libgcrypt20 libpcre2-8-0 libc-ares2 libxml2 zlib1g \
    && rm -rf /var/lib/apt/lists/*
COPY --from=wireshark /runtime/ /opt/wireshark/
ENV LD_LIBRARY_PATH=/opt/wireshark PATH=/opt/wireshark:$PATH PYTHONUNBUFFERED=1 PYTHONDONTWRITEBYTECODE=1
WORKDIR /opt/xcn-monitor
COPY monitor/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt
COPY monitor/ .
COPY --from=native /libxcn-trace.so /opt/xcn-monitor/libxcn-trace.so
USER 65532:65532
# Agents and decoder use explicit commands; all four roles share this image.
ENTRYPOINT ["python3", "/opt/xcn-monitor/server.py"]
