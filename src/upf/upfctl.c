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
#define RESPONSE_SIZE (4 * 1024 * 1024)
#define MAX_COLUMNS 16

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

static size_t split_columns(char *line, char **columns, size_t capacity)
{
    char *save = NULL;
    char *column;
    size_t count = 0;

    for (column = strtok_r(line, " \t\r", &save); column;
            column = strtok_r(NULL, " \t\r", &save)) {
        if (count == capacity)
            return capacity + 1;
        columns[count++] = column;
    }
    return count;
}

static bool is_number(const char *value)
{
    char *end = NULL;

    errno = 0;
    (void)strtod(value, &end);
    return !errno && end != value && *end == '\0';
}

static int print_table(char *response)
{
    size_t width[MAX_COLUMNS] = { 0 };
    bool numeric[MAX_COLUMNS];
    bool has_data[MAX_COLUMNS] = { false };
    char *copy;
    char *line;
    char *save = NULL;
    size_t max_columns = 0;
    size_t row = 0;
    size_t i;

    if (response[0] == '{' || !strncmp(response, "ERROR", 5)) {
        fputs(response, stdout);
        return 0;
    }
    for (i = 0; i < MAX_COLUMNS; i++)
        numeric[i] = true;
    copy = strdup(response);
    if (!copy) {
        perror("strdup");
        return 1;
    }
    for (line = strtok_r(copy, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *columns[MAX_COLUMNS];
        size_t count = split_columns(line, columns, MAX_COLUMNS);

        if (count > MAX_COLUMNS) {
            free(copy);
            fputs(response, stdout);
            return 0;
        }
        if (count > max_columns)
            max_columns = count;
        for (i = 0; i < count; i++) {
            size_t length = strlen(columns[i]);

            if (length > width[i])
                width[i] = length;
            if (row) {
                has_data[i] = true;
                if (!is_number(columns[i]))
                    numeric[i] = false;
            }
        }
        row++;
    }
    free(copy);

    save = NULL;
    row = 0;
    for (line = strtok_r(response, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *columns[MAX_COLUMNS];
        size_t count = split_columns(line, columns, MAX_COLUMNS);

        for (i = 0; i < count; i++) {
            if (i)
                fputs(" | ", stdout);
            if (has_data[i] && numeric[i])
                fprintf(stdout, "%*s", (int)width[i], columns[i]);
            else
                fprintf(stdout, "%-*s", (int)width[i], columns[i]);
        }
        fputc('\n', stdout);
        if (!row) {
            for (i = 0; i < max_columns; i++) {
                size_t j;

                if (i)
                    fputs("-+-", stdout);
                for (j = 0; j < width[i]; j++)
                    fputc('-', stdout);
            }
            fputc('\n', stdout);
        }
        row++;
    }
    return 0;
}

static int query(const char *socket_path, const char *request)
{
    struct sockaddr_un address;
    char *response;
    int fd;
    ssize_t length;
    size_t response_len = 0;
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
    response = malloc(RESPONSE_SIZE + 1);
    if (!response) {
        perror("malloc");
        close(fd);
        return 1;
    }
    while ((length = read(fd, response + response_len,
                    RESPONSE_SIZE - response_len)) != 0) {
        if (length < 0) {
            if (errno == EINTR)
                continue;
            perror("read");
            free(response);
            close(fd);
            return 1;
        }
        response_len += length;
        if (response_len == RESPONSE_SIZE) {
            fprintf(stderr, "response exceeds %u bytes\n", RESPONSE_SIZE);
            free(response);
            close(fd);
            return 1;
        }
    }
    close(fd);
    response[response_len] = '\0';
    if (print_table(response)) {
        free(response);
        return 1;
    }
    free(response);
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
    bool json_output = false;
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
            json_output = true;
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

        if (watch && !json_output)
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
