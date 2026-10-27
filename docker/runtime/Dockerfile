ARG UBUNTU_IMAGE=192.168.10.35/xnet/ubuntu:22.04
ARG VPP_IMAGE=192.168.10.35/xcn/vpp:26.06
FROM ${VPP_IMAGE} AS vpp-libs

FROM ${UBUNTU_IMAGE} AS build

ARG BUILD_CPUSET
ARG BUILD_JOBS=2
ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
RUN sh /opt/xcn-build/apt-sources.sh && apt-get update && \
    DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
        python3-pip \
        python3-setuptools \
        python3-wheel \
        ninja-build \
        build-essential \
        flex \
        bison \
        git \
        cmake \
        meson \
        libsctp-dev \
        libgnutls28-dev \
        libgcrypt-dev \
        libssl-dev \
        libmongoc-dev \
        libbson-dev \
        libyaml-dev \
        libmicrohttpd-dev \
        libcurl4-gnutls-dev \
        libnghttp2-dev \
        libtins-dev \
        libtalloc-dev \
        iproute2 \
        ca-certificates \
        netbase \
        pkg-config && \
    if apt-cache show libidn-dev > /dev/null 2>&1; then \
        apt-get install -y --no-install-recommends libidn-dev; \
    else \
        apt-get install -y --no-install-recommends libidn11-dev; \
    fi && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /xcn
COPY --from=vpp-libs /usr/local/include/libmemif.h /usr/local/include/libmemif.h
COPY --from=vpp-libs /usr/local/lib/libmemif.so /usr/local/lib/libmemif.so
COPY ./ /xcn
RUN set -eu; \
    if [ -n "$BUILD_CPUSET" ]; then taskset -pc "$BUILD_CPUSET" $$ >/dev/null; fi; \
    python3 docker/vendor.py verify runtime && \
    python3 docker/vendor.py unpack freeDiameter /xcn/subprojects && \
    python3 docker/vendor.py unpack prometheus-client-c /xcn/subprojects && \
    meson setup build --wrap-mode=nodownload --prefix=/usr --sysconfdir=/etc --localstatedir=/var \
        --buildtype=debugoptimized -Dupf_memif=true && \
    ninja -C build -j "$BUILD_JOBS" && \
    DESTDIR=/runtime-root ninja -C build install && \
    mkdir -p /runtime-root/etc/xcn /runtime-root/etc/freeDiameter && \
    cp -a build/configs/xcn/. /runtime-root/etc/xcn/ && \
    cp -a build/configs/freeDiameter/. /runtime-root/etc/freeDiameter/

FROM ${UBUNTU_IMAGE}

ARG OFFLINE=0
ARG APT_MIRROR
COPY docker/runtime/sources.list docker/apt-sources.sh /opt/xcn-build/

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
RUN sh /opt/xcn-build/apt-sources.sh && apt-get -o Acquire::Retries=3 update && \
    DEBIAN_FRONTEND=noninteractive \
    apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
        libsctp1 \
        libgnutls30 \
        libgcrypt20 \
        libssl3 \
        libmongoc-1.0-0 \
        libbson-1.0-0 \
        libyaml-0-2 \
        libmicrohttpd12 \
        libcurl3-gnutls \
        libnghttp2-14 \
        libtins4.0 \
        libtalloc2 \
        libidn12 \
        iputils-ping \
        iproute2 \
        iperf \
        iperf3 \
        tcpdump \
        iptables \
        netbase \
        netcat-openbsd \
        ca-certificates \
        binutils \
        procps \
        util-linux \
        python3 \
        openssl \
        xz-utils \
        bzip2 && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# Keep installed artifacts and unstripped symbols, not the builder's /usr tree.
COPY --from=build /runtime-root/ /
COPY --from=vpp-libs /usr/local/lib/libmemif.so /usr/local/lib/libmemif.so
COPY docker/runtime/setup.sh /usr/local/bin/xcn-k8s-setup
COPY docker/runtime/wait-for-tcp.sh /usr/local/bin/wait-for-tcp.sh
COPY misc/db/xcn-dbctl /usr/bin/xcn-dbctl

RUN chmod +x /usr/local/bin/xcn-k8s-setup /usr/local/bin/wait-for-tcp.sh /usr/bin/xcn-dbctl && \
    mkdir -p /etc/xcn /var/log/xcn && \
    ldconfig

WORKDIR /root
