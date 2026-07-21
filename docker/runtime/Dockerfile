FROM vpp:26.06 AS vpp-libs

FROM ubuntu:22.04 AS build

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
RUN apt-get update && \
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

WORKDIR /open5gs
COPY --from=vpp-libs /usr/local/include/libmemif.h /usr/local/include/libmemif.h
COPY --from=vpp-libs /usr/local/lib/libmemif.so /usr/local/lib/libmemif.so
COPY ./ /open5gs
RUN meson setup build --prefix=/usr --buildtype=debugoptimized \
        -Dupf_memif=true && \
    ninja -C build install

FROM ubuntu:22.04

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"
RUN apt-get update && \
    DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
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
        iputils-ping \
        iproute2 \
        iperf \
        iperf3 \
        tcpdump \
        iptables \
        netbase \
        netcat-openbsd \
        ca-certificates && \
    if apt-cache show libidn-dev > /dev/null 2>&1; then \
        apt-get install -y --no-install-recommends libidn-dev; \
    else \
        apt-get install -y --no-install-recommends libidn11-dev; \
    fi && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr /usr
COPY --from=vpp-libs /usr/local/lib/libmemif.so /usr/local/lib/libmemif.so
COPY docker/runtime/setup.sh /usr/local/bin/open5gs-k8s-setup
COPY docker/runtime/wait-for-tcp.sh /usr/local/bin/wait-for-tcp.sh

RUN chmod +x /usr/local/bin/open5gs-k8s-setup /usr/local/bin/wait-for-tcp.sh && \
    mkdir -p /etc/open5gs /var/log/open5gs && \
    ldconfig

WORKDIR /root
