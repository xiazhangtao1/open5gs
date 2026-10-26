# Build context: directory containing the locally built xcn Debian packages.
# docker build -f /path/to/xcn/docker/package/Dockerfile -t xcn-package /path/to/debs
ARG tag=latest
FROM ubuntu:${tag}

COPY *.deb /tmp/xcn-debs/
RUN apt-get update && \
    apt-get install -y /tmp/xcn-debs/*.deb && \
    rm -rf /tmp/xcn-debs /var/lib/apt/lists/*

WORKDIR /root
