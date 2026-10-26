ARG dist=ubuntu
ARG tag=latest
ARG username=acetcom
FROM ${username}/${dist}-${tag}-xcn-base

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"

WORKDIR /xcn
COPY docker/build/setup.sh /root
COPY ./ /xcn

ADD https://github.com/ufoscout/docker-compose-wait/releases/download/2.9.0/wait /wait
RUN chmod +x /wait

RUN meson build && ninja -C build install
