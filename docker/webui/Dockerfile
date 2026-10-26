FROM node:19

LABEL org.opencontainers.image.authors="Sukchan Lee <acetcom@gmail.com>"

COPY webui /usr/src/xcn/webui

WORKDIR /usr/src/xcn/webui
RUN command -v taskset && npm clean-install && \
    npm run build

ADD https://github.com/ufoscout/docker-compose-wait/releases/download/2.9.0/wait /wait
RUN chmod +x /wait

CMD /wait && npm run start

EXPOSE 9999
