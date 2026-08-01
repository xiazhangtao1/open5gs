/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET "/run/open5gs/upf-stats.sock"
#define REQUEST_SIZE 512

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: open5gs-upfctl show rate [options]\n"
        "  --level user|session|bearer|rule\n"
        "  --supi SUPI       Filter by SUPI (for example imsi-001010000000001)\n"
        "  --ue-ip ADDRESS   Filter by UE IP address\n"
        "  --seid SEID       Filter by UPF N4 SEID\n"
        "  --json            Emit JSON\n"
        "  --watch           Refresh continuously\n"
        "  --interval SEC    Watch interval (default 1)\n"
        "  --socket PATH     Control socket path\n");
}

static bool safe_value(const char *value)
{
    return value && *value && !strpbrk(value, " \t\r\n");
}

static int append_option(char *request, size_t size,
        const char *key, const char *value)
{
    size_t used = strlen(request);
    int written;

    if (!safe_value(value))
        return -1;
    written = snprintf(request + used, size - used, " %s=%s", key, value);
    if (written < 0 || (size_t)written >= size - used)
        return -1;
    return 0;
}

static int query(const char *socket_path, const char *request)
{
    struct sockaddr_un address;
    char buffer[8192];
    int fd;
    ssize_t length;
    size_t request_len = strlen(request);

    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "socket path is too long\n");
        return 1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "cannot connect to %s: %s\n",
                socket_path, strerror(errno));
        close(fd);
        return 1;
    }
    while (request_len) {
        length = write(fd, request, request_len);
        if (length > 0) {
            request += length;
            request_len -= length;
        } else if (length < 0 && errno == EINTR) {
            continue;
        } else {
            perror("write");
            close(fd);
            return 1;
        }
    }
    shutdown(fd, SHUT_WR);
    while ((length = read(fd, buffer, sizeof(buffer))) != 0) {
        size_t offset = 0;

        if (length < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            close(fd);
            return 1;
        }
        while (offset < (size_t)length) {
            size_t written = fwrite(buffer + offset, 1,
                    (size_t)length - offset, stdout);

            if (!written) {
                close(fd);
                return 1;
            }
            offset += written;
        }
    }
    close(fd);
    fflush(stdout);
    return 0;
}

int main(int argc, char *argv[])
{
    static const struct option options[] = {
        { "level", required_argument, NULL, 'l' },
        { "supi", required_argument, NULL, 'u' },
        { "ue-ip", required_argument, NULL, 'i' },
        { "seid", required_argument, NULL, 's' },
        { "json", no_argument, NULL, 'j' },
        { "watch", no_argument, NULL, 'w' },
        { "interval", required_argument, NULL, 'n' },
        { "socket", required_argument, NULL, 'S' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *socket_path = DEFAULT_SOCKET;
    char request[REQUEST_SIZE] = "show=rate";
    bool watch = false;
    double interval = 1.0;
    int option;

    if (argc < 3 || strcmp(argv[1], "show") || strcmp(argv[2], "rate")) {
        usage(stderr);
        return 2;
    }
    optind = 3;
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'l':
            if (strcmp(optarg, "user") && strcmp(optarg, "session") &&
                strcmp(optarg, "bearer") && strcmp(optarg, "rule")) {
                fprintf(stderr, "invalid level: %s\n", optarg);
                return 2;
            }
            if (append_option(request, sizeof(request), "level", optarg))
                return 2;
            break;
        case 'u':
            if (append_option(request, sizeof(request), "supi", optarg))
                return 2;
            break;
        case 'i':
            if (append_option(request, sizeof(request), "ue_ip", optarg))
                return 2;
            break;
        case 's':
        {
            char *end = NULL;

            errno = 0;
            (void)strtoull(optarg, &end, 0);
            if (errno || end == optarg || *end) {
                fprintf(stderr, "invalid SEID: %s\n", optarg);
                return 2;
            }
            if (append_option(request, sizeof(request), "seid", optarg))
                return 2;
            break;
        }
        case 'j':
            if (append_option(request, sizeof(request), "json", "1"))
                return 2;
            break;
        case 'w':
            watch = true;
            break;
        case 'n': {
            char *end = NULL;

            interval = strtod(optarg, &end);
            if (!end || *end || interval < 0.1 || interval > 3600.0) {
                fprintf(stderr, "invalid interval: %s\n", optarg);
                return 2;
            }
            break;
        }
        case 'S':
            socket_path = optarg;
            break;
        case 'h':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind != argc) {
        usage(stderr);
        return 2;
    }

    do {
        struct timespec delay;
        int rv;

        if (watch && isatty(STDOUT_FILENO))
            fputs("\033[H\033[J", stdout);
        rv = query(socket_path, request);
        if (!watch)
            return rv;
        delay.tv_sec = (time_t)interval;
        delay.tv_nsec = (long)((interval - delay.tv_sec) * 1000000000.0);
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
    } while (true);
}
