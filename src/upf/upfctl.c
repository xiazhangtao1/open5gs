/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <curl/curl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "sbi/openapi/external/cJSON.h"

#define DEFAULT_SOCKET "/run/open5gs/upf-stats.sock"
#define REQUEST_SIZE 512
#define RESPONSE_SIZE (4 * 1024 * 1024)
#define MAX_COLUMNS 18
#define DEFAULT_SMF_PDU_INFO_URL "http://127.0.0.1:9092/pdu-info"
#define DEFAULT_AMF_UE_INFO_URL "http://127.0.0.1:9091/ue-info"
#define PSI_MAP_MAX 4096
#define CM_STATE_MAP_MAX 4096
#define ENRICHED_RESPONSE_SIZE (RESPONSE_SIZE * 2)

typedef struct {
    char supi[64];
    char ue_ip[INET6_ADDRSTRLEN];
    unsigned int psi;
} psi_map_entry_t;

typedef struct {
    psi_map_entry_t entries[PSI_MAP_MAX];
    size_t count;
} psi_map_t;

typedef struct {
    char supi[64];
    char state[16];
} cm_state_map_entry_t;

typedef struct {
    cm_state_map_entry_t entries[CM_STATE_MAP_MAX];
    size_t count;
} cm_state_map_t;

typedef struct {
    char *data;
    size_t length;
} http_response_t;

static size_t split_columns(char *line, char **columns, size_t capacity);

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: xcnctl show rate [options]\n"
        "  --level user|session|bearer|rule\n"
        "  --supi SUPI       Filter by SUPI (for example imsi-001010000000001)\n"
        "  --ue-ip ADDRESS   Filter by UE IP address\n"
        "  --seid SEID       Filter by UPF N4 SEID\n"
        "  --smf-pdu-info URL  SMF /pdu-info URL used to resolve PSI\n"
        "  --amf-ue-info URL   AMF /ue-info URL used to resolve CM-STATE\n"
        "  --json            Emit JSON\n"
        "  --watch           Refresh continuously\n"
        "  --interval SEC    Watch interval (default 1)\n"
        "  --socket PATH     Control socket path\n");
}

static size_t http_write(void *data, size_t size, size_t count, void *opaque)
{
    http_response_t *response = opaque;
    size_t bytes = size * count;
    char *expanded;

    if (bytes > RESPONSE_SIZE - response->length)
        return 0;
    expanded = realloc(response->data, response->length + bytes + 1);
    if (!expanded)
        return 0;
    response->data = expanded;
    memcpy(response->data + response->length, data, bytes);
    response->length += bytes;
    response->data[response->length] = '\0';
    return bytes;
}

static int psi_map_add(psi_map_t *map, const char *supi,
        const char *ue_ip, unsigned int psi)
{
    psi_map_entry_t *entry;

    if (!supi || !*supi || !ue_ip || !*ue_ip || !psi ||
        map->count == PSI_MAP_MAX)
        return -1;
    entry = &map->entries[map->count++];
    snprintf(entry->supi, sizeof(entry->supi), "%s", supi);
    snprintf(entry->ue_ip, sizeof(entry->ue_ip), "%s", ue_ip);
    entry->psi = psi;
    return 0;
}

static void psi_map_parse_page(psi_map_t *map, const char *json,
        bool *has_next)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *items;
    cJSON *ue;
    cJSON *pager;

    *has_next = false;
    if (!root)
        return;
    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    cJSON_ArrayForEach(ue, items) {
        cJSON *supi = cJSON_GetObjectItemCaseSensitive(ue, "supi");
        cJSON *pdus = cJSON_GetObjectItemCaseSensitive(ue, "pdu");
        cJSON *pdu;

        if (!cJSON_IsString(supi) || !cJSON_IsArray(pdus))
            continue;
        cJSON_ArrayForEach(pdu, pdus) {
            cJSON *psi = cJSON_GetObjectItemCaseSensitive(pdu, "psi");
            cJSON *ipv4 = cJSON_GetObjectItemCaseSensitive(pdu, "ipv4");
            cJSON *ipv6 = cJSON_GetObjectItemCaseSensitive(pdu, "ipv6");

            if (!cJSON_IsNumber(psi) || psi->valuedouble < 1 ||
                psi->valuedouble > 255)
                continue;
            if (cJSON_IsString(ipv4))
                (void)psi_map_add(map, supi->valuestring,
                        ipv4->valuestring, (unsigned int)psi->valuedouble);
            if (cJSON_IsString(ipv6))
                (void)psi_map_add(map, supi->valuestring,
                        ipv6->valuestring, (unsigned int)psi->valuedouble);
        }
    }
    pager = cJSON_GetObjectItemCaseSensitive(root, "pager");
    if (cJSON_IsObject(pager))
        *has_next = cJSON_IsString(
                cJSON_GetObjectItemCaseSensitive(pager, "next"));
    cJSON_Delete(root);
}

static void load_psi_map(const char *base_url, psi_map_t *map)
{
    unsigned int page;

    memset(map, 0, sizeof(*map));
    for (page = 0; page < 1024 && map->count < PSI_MAP_MAX; page++) {
        CURL *curl = curl_easy_init();
        http_response_t response = { 0 };
        char url[1024];
        bool has_next = false;
        CURLcode result;
        long status = 0;

        if (!curl)
            break;
        snprintf(url, sizeof(url), "%s%cpage=%u&page_size=100", base_url,
                strchr(base_url, '?') ? '&' : '?', page);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 200L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        result = curl_easy_perform(curl);
        if (result == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK || status != 200 || !response.data) {
            free(response.data);
            break;
        }
        psi_map_parse_page(map, response.data, &has_next);
        free(response.data);
        if (!has_next)
            break;
    }
}

static bool psi_map_find(
        const psi_map_t *map, const char *supi, const char *ue_ip,
        unsigned int *psi)
{
    size_t i;
    bool found = false;

    for (i = 0; i < map->count; i++) {
        if (!strcmp(map->entries[i].supi, supi) &&
            !strcmp(map->entries[i].ue_ip, ue_ip)) {
            if (found && *psi != map->entries[i].psi)
                return false;
            *psi = map->entries[i].psi;
            found = true;
        }
    }
    return found;
}

static int cm_state_map_add(
        cm_state_map_t *map, const char *supi, const char *state)
{
    cm_state_map_entry_t *entry;
    size_t i;

    if (!supi || !*supi || !state ||
        (strcmp(state, "connected") && strcmp(state, "idle")))
        return -1;
    for (i = 0; i < map->count; i++) {
        entry = &map->entries[i];
        if (!strcmp(entry->supi, supi)) {
            if (!strcmp(state, "connected"))
                snprintf(entry->state, sizeof(entry->state), "%s", state);
            return 0;
        }
    }
    if (map->count == CM_STATE_MAP_MAX)
        return -1;
    entry = &map->entries[map->count++];
    snprintf(entry->supi, sizeof(entry->supi), "%s", supi);
    snprintf(entry->state, sizeof(entry->state), "%s", state);
    return 0;
}

static void cm_state_map_parse_page(
        cm_state_map_t *map, const char *json, bool *has_next)
{
    cJSON *root = cJSON_Parse(json);
    cJSON *items;
    cJSON *ue;
    cJSON *pager;

    *has_next = false;
    if (!root)
        return;
    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    cJSON_ArrayForEach(ue, items) {
        cJSON *supi = cJSON_GetObjectItemCaseSensitive(ue, "supi");
        cJSON *state = cJSON_GetObjectItemCaseSensitive(ue, "cm_state");

        if (cJSON_IsString(supi) && cJSON_IsString(state))
            (void)cm_state_map_add(
                    map, supi->valuestring, state->valuestring);
    }
    pager = cJSON_GetObjectItemCaseSensitive(root, "pager");
    if (cJSON_IsObject(pager))
        *has_next = cJSON_IsString(
                cJSON_GetObjectItemCaseSensitive(pager, "next"));
    cJSON_Delete(root);
}

static void load_cm_state_map(const char *base_url, cm_state_map_t *map)
{
    unsigned int page;

    memset(map, 0, sizeof(*map));
    for (page = 0; page < 1024 && map->count < CM_STATE_MAP_MAX; page++) {
        CURL *curl = curl_easy_init();
        http_response_t response = { 0 };
        char url[1024];
        bool has_next = false;
        CURLcode result;
        long status = 0;

        if (!curl)
            break;
        snprintf(url, sizeof(url), "%s%cpage=%u&page_size=100", base_url,
                strchr(base_url, '?') ? '&' : '?', page);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 200L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        result = curl_easy_perform(curl);
        if (result == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK || status != 200 || !response.data) {
            free(response.data);
            break;
        }
        cm_state_map_parse_page(map, response.data, &has_next);
        free(response.data);
        if (!has_next)
            break;
    }
}

static const char *cm_state_map_find(
        const cm_state_map_t *map, const char *supi)
{
    size_t i;

    for (i = 0; i < map->count; i++)
        if (!strcmp(map->entries[i].supi, supi))
            return map->entries[i].state;
    return "-";
}

static bool append_output(char *output, size_t capacity, size_t *used,
        const char *format, ...)
{
    va_list ap;
    int written;

    if (*used >= capacity)
        return false;
    va_start(ap, format);
    written = vsnprintf(output + *used, capacity - *used, format, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= capacity - *used)
        return false;
    *used += (size_t)written;
    return true;
}

static char *add_psi_column(const char *response, const psi_map_t *map)
{
    char *copy = strdup(response);
    char *output = malloc(ENRICHED_RESPONSE_SIZE + 1);
    char *line;
    char *save = NULL;
    size_t used = 0;
    bool header = true;
    bool session_table = false;

    if (!copy || !output) {
        free(copy);
        free(output);
        return NULL;
    }
    output[0] = '\0';
    for (line = strtok_r(copy, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *columns[MAX_COLUMNS];
        size_t count = split_columns(line, columns, MAX_COLUMNS);
        size_t i;

        if (header)
            session_table = count >= 3 && count <= MAX_COLUMNS &&
                !strcmp(columns[1], "UE-IP") &&
                !strcmp(columns[2], "SEID");
        if (!session_table || count < 3 || count > MAX_COLUMNS) {
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "%s\n", line))
                goto fail;
        } else if (header) {
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1, &used,
                        "%s %s PSI UPF-SEID", columns[0], columns[1]))
                goto fail;
            for (i = 3; i < count; i++) {
                if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                            &used, " %s", columns[i]))
                    goto fail;
            }
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "\n"))
                goto fail;
        } else {
            unsigned int psi;

            if (psi_map_find(map, columns[0], columns[1], &psi)) {
                if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1, &used,
                            "%s %s %u %s", columns[0], columns[1],
                            psi, columns[2]))
                    goto fail;
            } else if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "%s %s - %s", columns[0], columns[1],
                        columns[2])) {
                goto fail;
            }
            for (i = 3; i < count; i++) {
                if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                            &used, " %s", columns[i]))
                    goto fail;
            }
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "\n"))
                goto fail;
        }
        header = false;
    }
    free(copy);
    return output;

fail:
    free(copy);
    free(output);
    return NULL;
}

static char *add_psi_json(const char *response, const psi_map_t *map)
{
    cJSON *root = cJSON_Parse(response);
    cJSON *rows;
    cJSON *row;
    char *json;
    char *output = NULL;

    if (!root)
        return NULL;
    rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    cJSON_ArrayForEach(row, rows) {
        cJSON *supi = cJSON_GetObjectItemCaseSensitive(row, "supi");
        cJSON *ue_ip = cJSON_GetObjectItemCaseSensitive(row, "ue_ip");
        unsigned int psi;

        if (!cJSON_IsString(supi) || !cJSON_IsString(ue_ip))
            continue;
        if (psi_map_find(map, supi->valuestring, ue_ip->valuestring, &psi))
            cJSON_AddNumberToObject(row, "psi", psi);
        else
            cJSON_AddNullToObject(row, "psi");
    }
    json = cJSON_PrintUnformatted(root);
    if (json) {
        output = strdup(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    return output;
}

static char *add_cm_state_column(
        const char *response, const cm_state_map_t *map)
{
    char *copy = strdup(response);
    char *output = malloc(ENRICHED_RESPONSE_SIZE + 1);
    char *line;
    char *save = NULL;
    size_t used = 0;
    bool header = true;
    bool rate_table = false;

    if (!copy || !output) {
        free(copy);
        free(output);
        return NULL;
    }
    output[0] = '\0';
    for (line = strtok_r(copy, "\n", &save); line;
            line = strtok_r(NULL, "\n", &save)) {
        char *columns[MAX_COLUMNS];
        size_t count = split_columns(line, columns, MAX_COLUMNS);
        size_t i;

        if (header)
            rate_table = count && count <= MAX_COLUMNS &&
                !strcmp(columns[0], "SUPI");
        if (!rate_table || !count || count > MAX_COLUMNS) {
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "%s\n", line))
                goto fail;
        } else {
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1, &used,
                        "%s %s", columns[0],
                        header ? "CM-STATE" :
                        cm_state_map_find(map, columns[0])))
                goto fail;
            for (i = 1; i < count; i++) {
                if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                            &used, " %s", columns[i]))
                    goto fail;
            }
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "\n"))
                goto fail;
        }
        header = false;
    }
    free(copy);
    return output;

fail:
    free(copy);
    free(output);
    return NULL;
}

static char *add_cm_state_json(
        const char *response, const cm_state_map_t *map)
{
    cJSON *root = cJSON_Parse(response);
    cJSON *rows;
    cJSON *row;
    char *json;
    char *output = NULL;

    if (!root)
        return NULL;
    rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    cJSON_ArrayForEach(row, rows) {
        cJSON *supi = cJSON_GetObjectItemCaseSensitive(row, "supi");
        const char *state;

        if (!cJSON_IsString(supi))
            continue;
        state = cm_state_map_find(map, supi->valuestring);
        if (strcmp(state, "-"))
            cJSON_AddStringToObject(row, "cm_state", state);
        else
            cJSON_AddNullToObject(row, "cm_state");
    }
    json = cJSON_PrintUnformatted(root);
    if (json) {
        output = strdup(json);
        cJSON_free(json);
    }
    cJSON_Delete(root);
    return output;
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

static int query(const char *socket_path, const char *request,
        const char *smf_pdu_info_url, const char *amf_ue_info_url,
        bool include_psi, bool json_output)
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
    if (include_psi) {
        psi_map_t *map = malloc(sizeof(*map));
        char *enriched = NULL;

        if (!map) {
            perror("malloc");
            free(response);
            return 1;
        }
        load_psi_map(smf_pdu_info_url, map);
        if (json_output)
            enriched = add_psi_json(response, map);
        else
            enriched = add_psi_column(response, map);
        free(map);
        if (enriched) {
            free(response);
            response = enriched;
        }
    }
    {
        cm_state_map_t *map = malloc(sizeof(*map));
        char *enriched = NULL;

        if (!map) {
            perror("malloc");
            free(response);
            return 1;
        }
        load_cm_state_map(amf_ue_info_url, map);
        if (json_output)
            enriched = add_cm_state_json(response, map);
        else
            enriched = add_cm_state_column(response, map);
        free(map);
        if (enriched) {
            free(response);
            response = enriched;
        }
    }
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
        { "smf-pdu-info", required_argument, NULL, 'p' },
        { "amf-ue-info", required_argument, NULL, 'a' },
        { "json", no_argument, NULL, 'j' },
        { "watch", no_argument, NULL, 'w' },
        { "interval", required_argument, NULL, 'n' },
        { "socket", required_argument, NULL, 'S' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *socket_path = DEFAULT_SOCKET;
    const char *smf_pdu_info_url = getenv("OPEN5GS_SMF_PDU_INFO_URL");
    const char *amf_ue_info_url = getenv("OPEN5GS_AMF_UE_INFO_URL");
    char request[REQUEST_SIZE] = "show=rate";
    bool watch = false;
    bool json_output = false;
    bool include_psi = true;
    double interval = 1.0;
    int option;

    if (!smf_pdu_info_url || !*smf_pdu_info_url)
        smf_pdu_info_url = DEFAULT_SMF_PDU_INFO_URL;
    if (!amf_ue_info_url || !*amf_ue_info_url)
        amf_ue_info_url = DEFAULT_AMF_UE_INFO_URL;

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
            include_psi = strcmp(optarg, "user") != 0;
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
        case 'p':
            if (!strncmp(optarg, "http://", 7) ||
                !strncmp(optarg, "https://", 8))
                smf_pdu_info_url = optarg;
            else {
                fprintf(stderr, "invalid SMF PDU info URL: %s\n", optarg);
                return 2;
            }
            break;
        case 'a':
            if (!strncmp(optarg, "http://", 7) ||
                !strncmp(optarg, "https://", 8))
                amf_ue_info_url = optarg;
            else {
                fprintf(stderr, "invalid AMF UE info URL: %s\n", optarg);
                return 2;
            }
            break;
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
        rv = query(socket_path, request, smf_pdu_info_url, amf_ue_info_url,
                include_psi, json_output);
        if (!watch)
            return rv;
        delay.tv_sec = (time_t)interval;
        delay.tv_nsec = (long)((interval - delay.tv_sec) * 1000000000.0);
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
    } while (true);
}
