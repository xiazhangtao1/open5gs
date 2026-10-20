/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <curl/curl.h>
#include <getopt.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "sbi/openapi/external/cJSON.h"
#include "inspect.h"
#include "ue-events-cli.h"

#define DEFAULT_SOCKET "/run/open5gs/upf-stats.sock"
#define REQUEST_SIZE 512
#define RESPONSE_SIZE (4 * 1024 * 1024)
#define MAX_COLUMNS 24
#define DEFAULT_SMF_PDU_INFO_URL "http://127.0.0.1:9092/pdu-info"
#define DEFAULT_AMF_UE_INFO_URL "http://127.0.0.1:9091/ue-info"
#define DEFAULT_AMF_GNB_INFO_URL "http://127.0.0.1:9091/gnb-info"
#define PSI_MAP_MAX 4096
#define QFI_MAX 63
#define CM_STATE_MAP_MAX 4096
#define ACTIVE_SESSION_MAP_MAX 4096
#define ENRICHED_RESPONSE_SIZE (RESPONSE_SIZE * 2)

typedef struct {
    char supi[64];
    char ue_ip[INET6_ADDRSTRLEN];
    uint64_t upf_seid;
    bool upf_seid_present;
    unsigned int psi;
    uint8_t fiveqi[QFI_MAX + 1];
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
    char supi[64];
    unsigned int psi;
} active_session_map_entry_t;

typedef struct {
    cm_state_map_entry_t entries[CM_STATE_MAP_MAX];
    size_t count;
    active_session_map_entry_t active_sessions[ACTIVE_SESSION_MAP_MAX];
    size_t active_session_count;
    cJSON *ue_info;
} cm_state_map_t;

static size_t split_columns(char *line, char **columns, size_t capacity);

static void usage(FILE *stream)
{
    fprintf(stream,
        "Usage: xcnctl show rate|ue|gnb|ue-events [options]\n"
        "  ue-events        UE control-plane history (use show ue-events --help)\n"
        "  --detail          UE/gNB details, including UE session summaries\n"
        "  --gnb-id ID       UE/gNB filter (decimal or 0x hexadecimal)\n"
        "  --plmn MCCMNC     UE serving PLMN or gNB identity PLMN filter\n"
        "  --with-location   Add GNB-ID/GNB-LEN/CELL-ID/LOCATION to rate output\n"
        "  --amf-gnb-info URL AMF /gnb-info endpoint\n"
        "  --level user|session|bearer|rule\n"
        "  --supi SUPI       Filter by SUPI (for example imsi-001010000000001)\n"
        "  --ue-ip ADDRESS   Filter by UE IP address\n"
        "  --seid SEID       Filter by UPF N4 SEID\n"
        "  --smf-pdu-info URL  SMF /pdu-info URL used to resolve PSI\n"
        "  --amf-ue-info URL   AMF /ue-info URL used to resolve CM-STATE\n"
        "  --active-only     Show only Sessions currently held by the AMF\n"
        "  --json            Emit JSON\n"
        "  --watch           Refresh continuously\n"
        "  --interval SEC    Watch interval (default 1)\n"
        "  --socket PATH     Control socket path\n");
}

static bool parse_u64(const char *value, uint64_t *result)
{
    char *end = NULL;
    unsigned long long parsed;

    if (!value || !*value || !result)
        return false;
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno || end == value || *end)
        return false;
    *result = (uint64_t)parsed;
    return true;
}

static int psi_map_add(psi_map_t *map, const char *supi,
        const char *ue_ip, unsigned int psi,
        bool upf_seid_present, uint64_t upf_seid, const cJSON *qos_flows)
{
    psi_map_entry_t *entry;
    const cJSON *flow;

    if (!supi || !*supi || !ue_ip || !*ue_ip || !psi ||
        map->count == PSI_MAP_MAX)
        return -1;
    entry = &map->entries[map->count++];
    snprintf(entry->supi, sizeof(entry->supi), "%s", supi);
    snprintf(entry->ue_ip, sizeof(entry->ue_ip), "%s", ue_ip);
    entry->upf_seid = upf_seid;
    entry->upf_seid_present = upf_seid_present;
    entry->psi = psi;
    cJSON_ArrayForEach(flow, qos_flows) {
        const cJSON *qfi = cJSON_GetObjectItemCaseSensitive(flow, "qfi");
        const cJSON *fiveqi =
            cJSON_GetObjectItemCaseSensitive(flow, "5qi");

        if (!cJSON_IsNumber(qfi) || qfi->valuedouble < 1 ||
            qfi->valuedouble > QFI_MAX ||
            qfi->valuedouble > qfi->valueint ||
            !cJSON_IsNumber(fiveqi) || fiveqi->valuedouble < 1 ||
            fiveqi->valuedouble > UINT8_MAX ||
            fiveqi->valuedouble > fiveqi->valueint)
            continue;
        entry->fiveqi[qfi->valueint] = fiveqi->valueint;
    }
    return 0;
}

static bool load_psi_map(const char *base_url, psi_map_t *map)
{
    cJSON *root = xcnctl_load_info(base_url);
    const cJSON *ue;
    bool ok = root != NULL;

    memset(map, 0, sizeof(*map));
    cJSON_ArrayForEach(ue, cJSON_GetObjectItemCaseSensitive(root, "items")) {
        const cJSON *supi = cJSON_GetObjectItemCaseSensitive(ue, "supi");
        const cJSON *pdus = cJSON_GetObjectItemCaseSensitive(ue, "pdu");
        const cJSON *pdu;
        if (!cJSON_IsString(supi) || !cJSON_IsArray(pdus)) continue;
        cJSON_ArrayForEach(pdu, pdus) {
            const cJSON *psi = cJSON_GetObjectItemCaseSensitive(pdu, "psi");
            const cJSON *seid = cJSON_GetObjectItemCaseSensitive(pdu, "upf_seid");
            const cJSON *flows = cJSON_GetObjectItemCaseSensitive(pdu, "qos_flows");
            uint64_t id = 0;
            bool present = cJSON_IsString(seid) && parse_u64(seid->valuestring, &id);
            if (!cJSON_IsNumber(psi) || psi->valuedouble < 1 ||
                psi->valuedouble > 255 || psi->valuedouble > psi->valueint) continue;
            const char *keys[] = {"ipv4", "ipv6"};
            size_t i;
            for (i = 0; i < 2; i++) {
                const cJSON *ip = cJSON_GetObjectItemCaseSensitive(pdu, keys[i]);
                if (cJSON_IsString(ip) && ip->valuestring[0] &&
                    psi_map_add(map, supi->valuestring, ip->valuestring,
                        psi->valueint, present, id, flows) != 0) ok = false;
            }
        }
    }
    cJSON_Delete(root);
    if (!ok) memset(map, 0, sizeof(*map));
    return ok;
}

static bool psi_map_find(
        const psi_map_t *map, const char *supi, const char *ue_ip,
        uint64_t upf_seid, unsigned int *psi)
{
    size_t i;
    bool keyed = false;
    bool found = false;

    for (i = 0; i < map->count; i++) {
        if (!strcmp(map->entries[i].supi, supi) &&
            !strcmp(map->entries[i].ue_ip, ue_ip) &&
            map->entries[i].upf_seid_present) {
            keyed = true;
            if (map->entries[i].upf_seid != upf_seid)
                continue;
            if (found && *psi != map->entries[i].psi)
                return false;
            *psi = map->entries[i].psi;
            found = true;
        }
    }
    if (keyed)
        return found;

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

static bool fiveqi_map_find(
        const psi_map_t *map, const char *supi, const char *ue_ip,
        uint64_t upf_seid, unsigned int qfi, unsigned int *fiveqi)
{
    size_t i;
    bool keyed = false;
    bool found = false;

    if (!qfi || qfi > QFI_MAX)
        return false;
    for (i = 0; i < map->count; i++) {
        const psi_map_entry_t *entry = &map->entries[i];

        if (!strcmp(entry->supi, supi) && !strcmp(entry->ue_ip, ue_ip) &&
            entry->upf_seid_present) {
            keyed = true;
            if (entry->upf_seid != upf_seid || !entry->fiveqi[qfi])
                continue;
            if (found && *fiveqi != entry->fiveqi[qfi])
                return false;
            *fiveqi = entry->fiveqi[qfi];
            found = true;
        }
    }
    if (keyed)
        return found;

    for (i = 0; i < map->count; i++) {
        const psi_map_entry_t *entry = &map->entries[i];

        if (!strcmp(entry->supi, supi) && !strcmp(entry->ue_ip, ue_ip) &&
            entry->fiveqi[qfi]) {
            if (found && *fiveqi != entry->fiveqi[qfi])
                return false;
            *fiveqi = entry->fiveqi[qfi];
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

static int active_session_map_add(
        cm_state_map_t *map, const char *supi, unsigned int psi)
{
    active_session_map_entry_t *entry;
    size_t i;

    if (!supi || !*supi || !psi || psi > 255)
        return -1;
    for (i = 0; i < map->active_session_count; i++) {
        entry = &map->active_sessions[i];
        if (!strcmp(entry->supi, supi) && entry->psi == psi)
            return 0;
    }
    if (map->active_session_count == ACTIVE_SESSION_MAP_MAX)
        return -1;
    entry = &map->active_sessions[map->active_session_count++];
    snprintf(entry->supi, sizeof(entry->supi), "%s", supi);
    entry->psi = psi;
    return 0;
}

static bool load_cm_state_map(const char *base_url, cm_state_map_t *map)
{
    const cJSON *ue;
    bool ok;
    memset(map, 0, sizeof(*map));
    map->ue_info = xcnctl_load_info(base_url);
    ok = map->ue_info != NULL;
    cJSON_ArrayForEach(ue, cJSON_GetObjectItemCaseSensitive(map->ue_info, "items")) {
        const cJSON *supi = cJSON_GetObjectItemCaseSensitive(ue, "supi");
        const cJSON *state = cJSON_GetObjectItemCaseSensitive(ue, "cm_state");
        const cJSON *sessions = cJSON_GetObjectItemCaseSensitive(ue, "pdu_sessions");
        const cJSON *session;
        if (!cJSON_IsString(supi)) continue;
        if (cJSON_IsString(state) && cm_state_map_add(map,
                    supi->valuestring, state->valuestring) != 0) ok = false;
        cJSON_ArrayForEach(session, sessions) {
            const cJSON *psi = cJSON_GetObjectItemCaseSensitive(session, "psi");
            if (cJSON_IsNumber(psi) && psi->valuedouble >= 1 &&
                psi->valuedouble <= 255 && psi->valuedouble <= psi->valueint &&
                active_session_map_add(map, supi->valuestring, psi->valueint) != 0)
                ok = false;
        }
    }
    if (!ok) {
        cJSON_Delete(map->ue_info);
        memset(map, 0, sizeof(*map));
    }
    return ok;
}

static void cm_state_map_free(cm_state_map_t *map)
{
    if (map) cJSON_Delete(map->ue_info);
    free(map);
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

static bool active_session_map_find(
        const cm_state_map_t *map, const char *supi, unsigned int psi)
{
    size_t i;

    for (i = 0; i < map->active_session_count; i++)
        if (!strcmp(map->active_sessions[i].supi, supi) &&
            map->active_sessions[i].psi == psi)
            return true;
    return false;
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

static char *add_psi_column(
        const char *response, const psi_map_t *map,
        const cm_state_map_t *amf_map, bool active_only)
{
    char *copy = strdup(response);
    char *output = malloc(ENRICHED_RESPONSE_SIZE + 1);
    char *line;
    char *save = NULL;
    size_t used = 0;
    bool header = true;
    bool session_table = false;
    bool qfi_table = false;

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
        if (header && session_table)
            qfi_table = count >= 4 && !strcmp(columns[3], "QFI");
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
                if (qfi_table && i == 3 &&
                    !append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                            &used, " 5QI"))
                    goto fail;
            }
            if (!append_output(output, ENRICHED_RESPONSE_SIZE + 1,
                        &used, "\n"))
                goto fail;
        } else {
            uint64_t upf_seid;
            uint64_t qfi_value;
            unsigned int psi;
            unsigned int fiveqi = 0;
            bool psi_found = parse_u64(columns[2], &upf_seid) &&
                psi_map_find(map, columns[0], columns[1], upf_seid, &psi);
            bool fiveqi_found = qfi_table && psi_found &&
                parse_u64(columns[3], &qfi_value) &&
                qfi_value <= QFI_MAX && fiveqi_map_find(map,
                    columns[0], columns[1], upf_seid,
                    (unsigned int)qfi_value, &fiveqi);

            if (active_only && (!psi_found ||
                !active_session_map_find(amf_map, columns[0], psi)))
                continue;
            if (psi_found) {
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
                if (qfi_table && i == 3) {
                    if (fiveqi_found) {
                        if (!append_output(output,
                                    ENRICHED_RESPONSE_SIZE + 1,
                                    &used, " %u", fiveqi))
                            goto fail;
                    } else if (!append_output(output,
                                ENRICHED_RESPONSE_SIZE + 1,
                                &used, " -")) {
                        goto fail;
                    }
                }
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

static bool json_u64(const cJSON *item, uint64_t *value)
{
    if (cJSON_IsString(item))
        return parse_u64(item->valuestring, value);
    if (cJSON_IsNumber(item) && item->valuedouble >= 0 &&
        item->valuedouble <= 9007199254740991.0) {
        *value = (uint64_t)item->valuedouble;
        return !(item->valuedouble > (double)*value);
    }
    return false;
}

static char *add_psi_json(
        const char *response, const psi_map_t *map,
        const cm_state_map_t *amf_map, bool active_only)
{
    cJSON *root = cJSON_Parse(response);
    cJSON *rows;
    cJSON *row;
    char *json;
    char *output = NULL;

    if (!root)
        return NULL;
    rows = cJSON_GetObjectItemCaseSensitive(root, "rows");
    row = cJSON_IsArray(rows) ? rows->child : NULL;
    while (row) {
        cJSON *next = row->next;
        cJSON *supi = cJSON_GetObjectItemCaseSensitive(row, "supi");
        cJSON *ue_ip = cJSON_GetObjectItemCaseSensitive(row, "ue_ip");
        cJSON *seid = cJSON_GetObjectItemCaseSensitive(row, "seid");
        uint64_t upf_seid;
        uint64_t qfi_value;
        unsigned int psi, fiveqi;
        bool psi_found;

        if (!cJSON_IsString(supi) || !cJSON_IsString(ue_ip) ||
            !json_u64(seid, &upf_seid)) {
            if (active_only) {
                cJSON_Delete(cJSON_DetachItemViaPointer(rows, row));
                row = next;
                continue;
            }
            row = next;
            continue;
        }
        psi_found = psi_map_find(map, supi->valuestring,
                ue_ip->valuestring, upf_seid, &psi);
        if (active_only && (!psi_found ||
            !active_session_map_find(amf_map, supi->valuestring, psi))) {
            cJSON_Delete(cJSON_DetachItemViaPointer(rows, row));
            row = next;
            continue;
        }
        if (psi_found)
            cJSON_AddNumberToObject(row, "psi", psi);
        else
            cJSON_AddNullToObject(row, "psi");
        {
            cJSON *qfi = cJSON_GetObjectItemCaseSensitive(row, "qfi");

            if (json_u64(qfi, &qfi_value) && qfi_value <= QFI_MAX &&
                fiveqi_map_find(map, supi->valuestring,
                    ue_ip->valuestring, upf_seid,
                    (unsigned int)qfi_value, &fiveqi))
                cJSON_AddNumberToObject(row, "5qi", fiveqi);
            else if (qfi)
                cJSON_AddNullToObject(row, "5qi");
        }
        row = next;
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
        bool include_psi, bool active_only, bool json_output, bool with_location)
{
    struct sockaddr_un address;
    cm_state_map_t *amf_map;
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
    amf_map = malloc(sizeof(*amf_map));
    if (!amf_map) {
        perror("malloc");
        free(response);
        return 1;
    }
    if (!load_cm_state_map(amf_ue_info_url, amf_map) && active_only) {
        fprintf(stderr,
                "cannot load AMF UE info required by --active-only\n");
        cm_state_map_free(amf_map);
        free(response);
        return 1;
    }
    if (include_psi) {
        psi_map_t *map = malloc(sizeof(*map));
        char *enriched = NULL;

        if (!map) {
            perror("malloc");
            cm_state_map_free(amf_map);
            free(response);
            return 1;
        }
        if (!load_psi_map(smf_pdu_info_url, map) && active_only) {
            fprintf(stderr,
                    "cannot load SMF PDU info required by --active-only\n");
            free(map);
            cm_state_map_free(amf_map);
            free(response);
            return 1;
        }
        if (json_output)
            enriched = add_psi_json(
                    response, map, amf_map, active_only);
        else
            enriched = add_psi_column(
                    response, map, amf_map, active_only);
        free(map);
        if (!enriched) {
            fprintf(stderr, "cannot enrich UPF response with PSI\n");
            cm_state_map_free(amf_map);
            free(response);
            return 1;
        }
        free(response);
        response = enriched;
    }
    {
        char *enriched = NULL;

        if (json_output)
            enriched = add_cm_state_json(response, amf_map);
        else
            enriched = add_cm_state_column(response, amf_map);
        if (!enriched) {
            fprintf(stderr, "cannot enrich UPF response with CM state\n");
            cm_state_map_free(amf_map);
            free(response);
            return 1;
        }
        free(response);
        response = enriched;
    }
    {
        char *enriched = xcnctl_add_rate_access(response, amf_map->ue_info,
                json_output, with_location);
        if (!amf_map->ue_info)
            fputs("warning: AMF access information unavailable\n", stderr);
        if (!enriched) {
            cm_state_map_free(amf_map);
            free(response);
            fputs("cannot build rate access output\n", stderr);
            return 1;
        }
        free(response);
        response = enriched;
    }
    cm_state_map_free(amf_map);
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
    if (argc >= 3 && !strcmp(argv[1], "show") && !strcmp(argv[2], "ue-events"))
        return xcnctl_ue_events_main(argc, argv);
    static const struct option options[] = {
        { "level", required_argument, NULL, 'l' },
        { "supi", required_argument, NULL, 'u' },
        { "ue-ip", required_argument, NULL, 'i' },
        { "seid", required_argument, NULL, 's' },
        { "smf-pdu-info", required_argument, NULL, 'p' },
        { "amf-ue-info", required_argument, NULL, 'a' },
        { "active-only", no_argument, NULL, 'A' },
        { "json", no_argument, NULL, 'j' },
        { "watch", no_argument, NULL, 'w' },
        { "interval", required_argument, NULL, 'n' },
        { "socket", required_argument, NULL, 'S' },
        { "detail", no_argument, NULL, 'd' },
        { "gnb-id", required_argument, NULL, 'g' },
        { "plmn", required_argument, NULL, 'm' },
        { "amf-gnb-info", required_argument, NULL, 'G' },
        { "with-location", no_argument, NULL, 'L' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *socket_path = DEFAULT_SOCKET;
    const char *smf_pdu_info_url = getenv("OPEN5GS_SMF_PDU_INFO_URL");
    const char *amf_ue_info_url = getenv("OPEN5GS_AMF_UE_INFO_URL");
    const char *amf_gnb_info_url = getenv("OPEN5GS_AMF_GNB_INFO_URL");
    xcnctl_inspect_options_t inspect = {0};
    bool is_rate;
    bool rate_options = false;
    bool with_location = false;
    char request[REQUEST_SIZE] = "show=rate";
    bool watch = false;
    bool json_output = false;
    bool include_psi = false;
    bool active_only = false;
    const char *level = "user";
    double interval = 1.0;
    int option;

    if (!smf_pdu_info_url || !*smf_pdu_info_url)
        smf_pdu_info_url = DEFAULT_SMF_PDU_INFO_URL;
    if (!amf_ue_info_url || !*amf_ue_info_url)
        amf_ue_info_url = DEFAULT_AMF_UE_INFO_URL;

    if (!amf_gnb_info_url || !*amf_gnb_info_url)
        amf_gnb_info_url = DEFAULT_AMF_GNB_INFO_URL;
    if (argc < 3 || strcmp(argv[1], "show") ||
        (strcmp(argv[2], "rate") && strcmp(argv[2], "ue") && strcmp(argv[2], "gnb"))) {
        usage(stderr);
        return 2;
    }
    is_rate = !strcmp(argv[2], "rate");
    optind = 3;
    while ((option = getopt_long(argc, argv, "", options, NULL)) != -1) {
        switch (option) {
        case 'l':
            rate_options = true;
            if (strcmp(optarg, "user") && strcmp(optarg, "session") &&
                strcmp(optarg, "bearer") && strcmp(optarg, "rule")) {
                fprintf(stderr, "invalid level: %s\n", optarg);
                return 2;
            }
            if (append_option(request, sizeof(request), "level", optarg))
                return 2;
            level = optarg;
            include_psi = strcmp(optarg, "user") != 0;
            break;
        case 'u':
            inspect.supi = optarg;
            if (append_option(request, sizeof(request), "supi", optarg))
                return 2;
            break;
        case 'i':
            rate_options = true;
            if (append_option(request, sizeof(request), "ue_ip", optarg))
                return 2;
            break;
        case 's':
        {
            char *end = NULL;
            rate_options = true;

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
        case 'A':
            rate_options = true;
            active_only = true;
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
            if (end == optarg || *end || !isfinite(interval) ||
                interval < 0.1 || interval > 3600.0) {
                fprintf(stderr, "invalid interval: %s\n", optarg);
                return 2;
            }
            break;
        }
        case 'S':
            rate_options = true;
            socket_path = optarg;
            break;
        case 'd':
            inspect.detail = true;
            break;
        case 'g': {
            char *end = NULL;
            errno = 0;
            unsigned long long id = strtoull(optarg, &end,
                    !strncasecmp(optarg, "0x", 2) ? 16 : 10);
            if (errno || !*optarg || *optarg == '-' || *optarg == '+' ||
                end == optarg || *end || id > UINT32_MAX) {
                fputs("invalid gNB ID\n", stderr);
                return 2;
            }
            inspect.gnb_id = id;
            inspect.has_gnb_id = true;
            break;
        }
        case 'm':
            if ((strlen(optarg) != 5 && strlen(optarg) != 6) ||
                strspn(optarg, "0123456789") != strlen(optarg)) {
                fputs("invalid PLMN (expected MCCMNC)\n", stderr);
                return 2;
            }
            inspect.plmn = optarg;
            break;
        case 'G':
            if (strncmp(optarg, "http://", 7) && strncmp(optarg, "https://", 8)) {
                fputs("invalid AMF gNB info URL\n", stderr);
                return 2;
            }
            amf_gnb_info_url = optarg;
            break;
        case 'L':
            with_location = true;
            rate_options = true;
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
    if ((!is_rate && rate_options) ||
        (is_rate && (inspect.detail || inspect.has_gnb_id || inspect.plmn)) ||
        (!strcmp(argv[2], "gnb") && inspect.supi)) {
        fputs("option does not apply to this command\n", stderr);
        return 2;
    }
    inspect.ue_url = amf_ue_info_url;
    inspect.gnb_url = amf_gnb_info_url;
    inspect.smf_url = smf_pdu_info_url;
    inspect.json = json_output;
    if (active_only && !strcmp(level, "user")) {
        fprintf(stderr,
                "--active-only requires --level session, bearer, or rule\n");
        return 2;
    }

    do {
        struct timespec delay;
        int rv;

        if (watch && !json_output)
            fputs("\033[H\033[J", stdout);
        rv = is_rate ? query(socket_path, request, smf_pdu_info_url, amf_ue_info_url,
                include_psi, active_only, json_output, with_location) :
            xcnctl_inspect(argv[2], &inspect);
        if (!watch)
            return rv;
        delay.tv_sec = (time_t)interval;
        delay.tv_nsec = (long)((interval - delay.tv_sec) * 1000000000.0);
        while (nanosleep(&delay, &delay) < 0 && errno == EINTR)
            ;
    } while (true);
}
