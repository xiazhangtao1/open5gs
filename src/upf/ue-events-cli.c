/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "ue-events-cli.h"
#include "sbi/openapi/external/cJSON.h"
#include <curl/curl.h>
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define EVENT_BYTES (4 * 1024 * 1024)
#define SEEN_SIZE 8192
typedef struct {
    const char *nf, *url;
    char boot[64];
    uint64_t cursor, seen[SEEN_SIZE];
    size_t seen_next;
    bool initialized;
} source_t;
typedef struct { char *data; size_t size; } body_t;
typedef struct {
    const char *supi, *trace_nf, *trace_id;
    char trace_boot[64];
    uint64_t since;
    size_t limit;
    bool detail, json, watch;
    double interval;
} options_t;

static const cJSON *get(const cJSON *o, const char *key) { return cJSON_GetObjectItemCaseSensitive(o, key); }
static const char *str(const cJSON *o, const char *key)
{
    const cJSON *v = get(o, key);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : "-";
}
static bool u64(const char *s, uint64_t *v)
{
    char *end;
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0; *v = strtoull(s, &end, 10);
    return !errno && !*end;
}
static bool epoch_valid(const char *s)
{
    if (strlen(s) != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
        else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}
static size_t receive(void *ptr, size_t size, size_t count, void *opaque)
{
    body_t *body = opaque;
    if (size && count > SIZE_MAX / size) return 0;
    size_t bytes = size * count;
    if (bytes > EVENT_BYTES - body->size) return 0;
    char *next = realloc(body->data, body->size + bytes + 1);
    if (!next) return 0;
    body->data = next; memcpy(next + body->size, ptr, bytes);
    body->size += bytes; next[body->size] = '\0';
    return bytes;
}

static cJSON *fetch(source_t *source, const options_t *o, const char *trace, bool backfill)
{
    CURL *curl = curl_easy_init();
    body_t body = {0};
    char url[4096], cursor[180] = "";
    char *supi = NULL, *trace_id = NULL;
    long status = 0;
    cJSON *doc = NULL;
    if (!curl) return NULL;
    if (o->supi && !backfill) supi = curl_easy_escape(curl, o->supi, 0);
    if (trace) trace_id = curl_easy_escape(curl, trace, 0);
    if ((o->supi && !backfill && !supi) || (trace && !trace_id)) goto done;
    if (source->initialized && !backfill)
        snprintf(cursor, sizeof(cursor), "&after=%" PRIu64 "&boot_id=%s", source->cursor, source->boot);
    const char *epoch = o->trace_nf ? o->trace_boot : source->boot;
    int n = snprintf(url, sizeof(url), "%s%climit=%zu&detail=%d&since_us=%" PRIu64 "%s%s%s%s%s%s%s",
            source->url, strchr(source->url, '?') ? '&' : '?',
            backfill || source->initialized ? 1000 : o->limit, o->detail, o->since, cursor,
            supi ? "&supi=" : "", supi ? supi : "",
            trace_id ? "&trace_id=" : "", trace_id ? trace_id : "",
            trace_id ? "&trace_boot_id=" : "", trace_id ? epoch : "");
    if (n < 0 || (size_t)n >= sizeof(url) || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))) goto done;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 200L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (curl_easy_perform(curl) != CURLE_OK) goto done;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != 200 || !body.data || memchr(body.data, '\0', body.size)) goto done;
    doc = cJSON_ParseWithOpts(body.data, NULL, true);
    uint64_t seq, next, latest;
    const char *boot = str(doc, "boot_id");
    const cJSON *items = get(doc, "items"), *item;
    bool valid = !strcmp(str(doc, "nf"), source->nf) && epoch_valid(boot) &&
        cJSON_IsArray(items) && cJSON_GetArraySize(items) <= 1000 &&
        cJSON_IsBool(get(doc, "has_more")) && cJSON_IsBool(get(doc, "reset")) &&
        cJSON_IsBool(get(doc, "gap")) && cJSON_IsBool(get(doc, "enabled")) &&
        u64(str(doc, "next_seq"), &next) && u64(str(doc, "latest_seq"), &latest) && next <= latest;
    bool incremental = source->initialized && !backfill && !strcmp(source->boot, boot);
    if (valid && incremental && next < source->cursor) valid = false;
    uint64_t previous = 0;
    cJSON_ArrayForEach(item, items) {
        const cJSON *ts = get(item, "timestamp_us");
        valid = valid && cJSON_IsObject(item) && u64(str(item, "seq"), &seq) && seq > previous && seq <= latest &&
            cJSON_IsNumber(ts) && isfinite(ts->valuedouble) && ts->valuedouble >= 0 && ts->valuedouble <= 9007199254740991.0 &&
            cJSON_IsBool(get(item, "action")) && cJSON_IsString(get(item, "event_id")) &&
            cJSON_IsString(get(item, "trace_id")) && cJSON_IsString(get(item, "protocol")) &&
            cJSON_IsString(get(item, "name")) && cJSON_IsString(get(item, "result")) &&
            cJSON_IsString(get(item, "direction")) && cJSON_IsString(get(item, "supi")) &&
            (!incremental || seq > source->cursor);
        const cJSON *order = get(item, "order_timestamp_us");
        if (order && (!cJSON_IsNumber(order) || !isfinite(order->valuedouble) ||
                    order->valuedouble < 0 || order->valuedouble > 9007199254740991.0)) valid = false;
        if (!valid) break;
        previous = seq;
    }
    if (!valid) { cJSON_Delete(doc); doc = NULL; }
done:
    free(body.data); curl_free(supi); curl_free(trace_id); curl_easy_cleanup(curl);
    return doc;
}

static bool add_events(cJSON *output, const cJSON *doc, source_t *source, bool initial, const options_t *o)
{
    const cJSON *item;
    cJSON_ArrayForEach(item, get(doc, "items")) {
        uint64_t seq;
        if (!u64(str(item, "seq"), &seq)) return false;
        bool seen = false;
        for (size_t i = 0; i < SEEN_SIZE; i++) if (source->seen[i] == seq) { seen = true; break; }
        if (seen) continue;
        if (!strcmp(str(item, "protocol"), "IDENTITY")) {
            if (!initial && o->supi) {
                cJSON *history = fetch(source, o, str(item, "trace_id"), true);
                if (!history) { fputs("warning: identity backfill unavailable\n", stderr); return false; }
                bool ok = add_events(output, history, source, true, o);
                cJSON_Delete(history);
                if (!ok) return false;
            }
            if (!o->detail || o->supi) {
                source->seen[source->seen_next++ % SEEN_SIZE] = seq;
                continue;
            }
        }
        source->seen[source->seen_next++ % SEEN_SIZE] = seq;
        if (!o->detail && !cJSON_IsTrue(get(item, "action"))) continue;
        cJSON *copy = cJSON_Duplicate(item, true);
        if (!copy) return false;
        if (!cJSON_AddStringToObject(copy, "nf", source->nf) || !cJSON_AddStringToObject(copy, "boot_id", source->boot) ||
            !cJSON_AddItemToArray(output, copy)) { cJSON_Delete(copy); return false; }
    }
    return true;
}

static int compare_events(const void *a, const void *b)
{
    const cJSON *left = *(const cJSON *const *)a, *right = *(const cJSON *const *)b;
    const cJSON *lt = get(left, "order_timestamp_us"), *rt = get(right, "order_timestamp_us");
    double x = cJSON_IsNumber(lt) ? lt->valuedouble : get(left, "timestamp_us")->valuedouble;
    double y = cJSON_IsNumber(rt) ? rt->valuedouble : get(right, "timestamp_us")->valuedouble;
    if (x < y || x > y) return x < y ? -1 : 1;
    int nf = strcmp(str(left, "nf"), str(right, "nf"));
    if (nf) return nf;
    uint64_t aseq = 0, bseq = 0; u64(str(left, "seq"), &aseq); u64(str(right, "seq"), &bseq);
    return aseq < bseq ? -1 : aseq > bseq;
}

static void safe_text(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) putchar(*p < 32 || *p == 127 ? '?' : *p);
}

static const char *numeric(const cJSON *item, const char *key, char *buf, size_t size)
{
    const cJSON *v = get(item, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble)) return "-";
    snprintf(buf, size, "%.0f", v->valuedouble);
    return buf;
}

static void column(const char *value, size_t width, bool right)
{
    size_t length = strlen(value);
    if (right) for (size_t n = length; n < width; n++) putchar(' ');
    safe_text(value);
    if (!right) for (size_t n = length; n < width; n++) putchar(' ');
    fputs(" | ", stdout);
}

#define EVENT_COLUMNS 8
static const char *event_headers[EVENT_COLUMNS] = {
    "TIME(UTC)", "NF", "SUPI", "PSI", "PROTOCOL", "DIRECTION", "EVENT", "RESULT"
};

static void event_columns(const cJSON *item, const char **values,
        char *timestamp, size_t size, char *number, size_t number_size)
{
    uint64_t stamp = get(item, "timestamp_us")->valuedouble;
    time_t seconds = stamp / 1000000;
    struct tm utc;
    char date[32] = "-";
    if (gmtime_r(&seconds, &utc)) strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc);
    snprintf(timestamp, size, "%s.%03uZ", date, (unsigned)((stamp / 1000) % 1000));
    values[0] = timestamp; values[1] = str(item, "nf"); values[2] = str(item, "supi");
    values[3] = numeric(item, "psi", number, number_size); values[4] = str(item, "protocol");
    values[5] = str(item, "direction"); values[6] = str(item, "name"); values[7] = str(item, "result");
}

static void print_events(const cJSON *items, size_t *widths, bool initial)
{
    const cJSON *item;
    const char *values[EVENT_COLUMNS];
    char timestamp[48], number[32];
    bool widened = false;
    for (size_t i = 0; i < EVENT_COLUMNS; i++)
        if (!widths[i]) widths[i] = strlen(event_headers[i]);
    cJSON_ArrayForEach(item, items) {
        event_columns(item, values, timestamp, sizeof(timestamp), number, sizeof(number));
        for (size_t i = 0; i < EVENT_COLUMNS; i++) {
            size_t length = strlen(values[i]);
            if (length > widths[i]) { widths[i] = length; widened = true; }
        }
    }
    /* Keep watch columns stable; emit a new header only when a column grows. */
    if (initial || widened) {
        for (size_t i = 0; i < EVENT_COLUMNS; i++) column(event_headers[i], widths[i], i == 3);
        puts("DETAILS");
        for (size_t i = 0; i < EVENT_COLUMNS; i++) {
            for (size_t n = 0; n < widths[i]; n++) putchar('-');
            fputs("-+-", stdout);
        }
        puts("-------");
    }
    cJSON_ArrayForEach(item, items) {
        event_columns(item, values, timestamp, sizeof(timestamp), number, sizeof(number));
        for (size_t i = 0; i < EVENT_COLUMNS; i++) column(values[i], widths[i], i == 3);
        const char *cause = str(item, "cause"), *info = str(item, "info");
        bool has_cause = strcmp(cause, "-") != 0, has_info = strcmp(info, "-") != 0;
        if (has_cause) safe_text(cause);
        if (has_cause && has_info) fputs("; ", stdout);
        if (has_info) safe_text(info);
        if (!has_cause && !has_info) putchar('-');
        putchar('\n');
    }
}

static int query_events(source_t *sources, const options_t *o, size_t *widths)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *all = root ? cJSON_AddArrayToObject(root, "items") : NULL;
    cJSON *metadata = root ? cJSON_AddArrayToObject(root, "sources") : NULL;
    if (!all || !metadata) { cJSON_Delete(root); return 1; }
    size_t available = 0;
    bool initial = !sources[0].initialized && !sources[1].initialized, partial = false;
    for (size_t n = 0; n < 2; n++) {
        source_t *s = &sources[n];
        if (o->trace_nf && strcmp(o->trace_nf, s->nf)) continue;
        /* Bound one refresh; any remaining backlog stays behind its cursor. */
        for (size_t page = 0; page < 4; page++) {
            bool first = !s->initialized;
            cJSON *doc = fetch(s, o, o->trace_id, false);
            if (!doc) {
                fprintf(stderr, "warning: %s UE event history unavailable or invalid\n", s->nf);
                partial = true; break;
            }
            if (page == 0) available++;
            const char *boot = str(doc, "boot_id");
            if (strcmp(s->boot, boot)) {
                if (s->initialized) fprintf(stderr, "warning: %s restarted; in-memory history reset\n", s->nf);
                memset(s->seen, 0, sizeof(s->seen)); s->seen_next = 0;
                snprintf(s->boot, sizeof(s->boot), "%s", boot);
            }
            if (cJSON_IsTrue(get(doc, "gap"))) fprintf(stderr, "warning: %s history window advanced; some events may have been overwritten\n", s->nf);
            if (!cJSON_IsTrue(get(doc, "enabled"))) fprintf(stderr, "warning: %s UE event history disabled\n", s->nf);
            bool ok = add_events(all, doc, s, first, o);
            uint64_t next = 0; u64(str(doc, "next_seq"), &next);
            bool more = !first && !cJSON_IsTrue(get(doc, "reset")) && cJSON_IsTrue(get(doc, "has_more"));
            if (more && next <= s->cursor) ok = false;
            if (ok) { s->cursor = next; s->initialized = true; }
            cJSON_DeleteItemFromObjectCaseSensitive(doc, "items");
            if (!cJSON_AddItemToArray(metadata, doc)) { cJSON_Delete(doc); ok = false; }
            if (!ok) { partial = true; break; }
            if (!more) break;
        }
    }
    int count = cJSON_GetArraySize(all);
    cJSON **sorted = calloc(count ? count : 1, sizeof(*sorted));
    if (!sorted) { cJSON_Delete(root); return 1; }
    cJSON *item; size_t i = 0;
    while ((item = cJSON_DetachItemFromArray(all, 0))) sorted[i++] = item;
    qsort(sorted, count, sizeof(*sorted), compare_events);
    for (i = 0; i < (size_t)count; i++) {
        if (initial && (size_t)count-i > o->limit) cJSON_Delete(sorted[i]);
        else cJSON_AddItemToArray(all, sorted[i]);
    }
    free(sorted);
    cJSON_AddBoolToObject(root, "partial", partial);
    if (o->json) {
        char *json = cJSON_PrintUnformatted(root);
        if (!json) { cJSON_Delete(root); return 1; }
        puts(json); cJSON_free(json);
    } else {
        print_events(all, widths, initial);
        if (initial && !cJSON_GetArraySize(all) && available)
            puts("(no retained events; history starts when recording is enabled and resets on NF restart)");
    }
    cJSON_Delete(root); fflush(stdout);
    return available ? 0 : 1;
}

static bool parse_since(const char *text, uint64_t *since)
{
    size_t n = strlen(text);
    if (n > 1 && strchr("smhd", text[n-1])) {
        char value[24]; uint64_t amount;
        if (n >= sizeof(value)) return false;
        memcpy(value, text, n-1); value[n-1] = '\0';
        if (!u64(value, &amount)) return false;
        uint64_t multiplier = text[n-1] == 's' ? 1 : text[n-1] == 'm' ? 60 : text[n-1] == 'h' ? 3600 : 86400;
        uint64_t now = (uint64_t)time(NULL) * 1000000;
        if (amount > UINT64_MAX / multiplier / 1000000) return false;
        amount *= multiplier * 1000000; *since = amount > now ? 0 : now - amount;
        return true;
    }
    struct tm tm = {0};
    char *end = strptime(text, "%Y-%m-%dT%H:%M:%SZ", &tm), normalized[32];
    if (!end || *end) return false;
    time_t stamp = timegm(&tm);
    strftime(normalized, sizeof(normalized), "%Y-%m-%dT%H:%M:%SZ", &tm);
    if (stamp < 0 || strcmp(normalized, text)) return false;
    *since = (uint64_t)stamp * 1000000; return true;
}

int xcnctl_ue_events_main(int argc, char **argv)
{
    static const struct option flags[] = {
        {"supi", required_argument, NULL, 's'}, {"trace-id", required_argument, NULL, 't'},
        {"detail", no_argument, NULL, 'd'}, {"watch", no_argument, NULL, 'w'},
        {"json", no_argument, NULL, 'j'}, {"since", required_argument, NULL, 'S'},
        {"limit", required_argument, NULL, 'l'}, {"interval", required_argument, NULL, 'i'},
        {"amf-ue-events", required_argument, NULL, 'a'}, {"smf-ue-events", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'}, {NULL, 0, NULL, 0}
    };
    options_t o = {.limit = 100, .interval = 1};
    size_t widths[EVENT_COLUMNS] = {0};
    source_t *sources = calloc(2, sizeof(*sources));
    char *trace = NULL;
    int result = 2, option;
    if (!sources) return 1;
    sources[0].nf = "AMF"; sources[1].nf = "SMF";
    sources[0].url = getenv("OPEN5GS_AMF_UE_EVENTS_URL");
    sources[1].url = getenv("OPEN5GS_SMF_UE_EVENTS_URL");
    if (!sources[0].url) sources[0].url = "http://127.0.0.1:9091/ue-events";
    if (!sources[1].url) sources[1].url = "http://127.0.0.1:9092/ue-events";
    optind = 3;
    while ((option = getopt_long(argc, argv, "", flags, NULL)) != -1) {
        uint64_t value;
        switch (option) {
        case 's': if (!*optarg || strlen(optarg) >= 64 || strpbrk(optarg, " \t\r\n")) goto done; o.supi = optarg; break;
        case 't': {
            free(trace); trace = strdup(optarg);
            if (!trace) { result = 1; goto done; }
            char *first = strchr(trace, '/'), *second = first ? strchr(first+1, '/') : NULL;
            if (!first || !second) goto done;
            *first++ = '\0'; *second++ = '\0';
            if ((strcmp(trace, "AMF") && strcmp(trace, "SMF")) || !epoch_valid(first) || !u64(second, &value) || !value) goto done;
            o.trace_nf = trace; o.trace_id = second;
            snprintf(o.trace_boot, sizeof(o.trace_boot), "%s", first); break;
        }
        case 'd': o.detail = true; break;
        case 'w': o.watch = true; break;
        case 'j': o.json = true; break;
        case 'S': if (!parse_since(optarg, &o.since)) goto done; break;
        case 'l': if (!u64(optarg, &value) || !value || value > 1000) goto done; o.limit = value; break;
        case 'i': {
            char *end; errno = 0; o.interval = strtod(optarg, &end);
            if (errno || !*optarg || *end || !isfinite(o.interval) || o.interval < 0.1 || o.interval > 3600) goto done;
            break;
        }
        case 'a': sources[0].url = optarg; break;
        case 'm': sources[1].url = optarg; break;
        case 'h':
            puts("Usage: xcnctl show ue-events [--supi SUPI | --trace-id NF/BOOT/ID] [--detail] [--watch] [--json]\n"
                 "  --detail             Include NGAP/NAS/PFCP steps; one text line per event\n"
                 "  --since 30m|UTC-TIME  Filter event time (for example 2026-10-20T12:00:00Z)\n"
                 "  --limit N            Initial history limit, 1..1000 (default 100)\n"
                 "  --interval SEC       Watch interval, 0.1..3600 (default 1)\n"
                 "  --amf-ue-events URL  AMF /ue-events endpoint\n"
                 "  --smf-ue-events URL  SMF /ue-events endpoint\n"
                 "Records are bounded in memory, survive UE release and reset on NF restart.");
            result = 0; goto done;
        default: goto done;
        }
    }
    if (optind != argc || (o.supi && o.trace_id)) goto done;
    do {
        result = query_events(sources, &o, widths);
        if (!o.watch) break;
        struct timespec delay = {(time_t)o.interval, (long)((o.interval - (time_t)o.interval)*1000000000)};
        while (nanosleep(&delay, &delay) && errno == EINTR) {}
    } while (true);
done:
    if (result == 2) fputs("invalid ue-events options; use --help\n", stderr);
    free(trace); free(sources); return result;
}
