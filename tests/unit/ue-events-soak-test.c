/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "proto/ue-events.h"
#include "sbi/openapi/external/cJSON.h"
#include <inttypes.h>
#ifdef __GLIBC__
#include <malloc.h>
#if __GLIBC_PREREQ(2, 33)
#define HAVE_MALLINFO2 1
#endif
#endif
#ifndef __has_feature
#define __has_feature(feature) 0
#endif
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define USERS 4096
typedef struct { const char *supi; char cursor[32]; bool paging; } query_t;
static atomic_bool stopped;
static atomic_uint_fast64_t reads;

static const char *argument(void *request, const char *key)
{
    query_t *q = request;
    if (!strcmp(key, "detail")) return "1";
    if (!strcmp(key, "limit")) return "1000";
    if (!strcmp(key, "supi")) return q->supi;
    if (!strcmp(key, "after") && q->paging) return q->cursor;
    return NULL;
}

static const cJSON *field(const cJSON *o, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static uint64_t number(const cJSON *o, const char *key)
{
    const cJSON *v = field(o, key);
    ogs_assert(cJSON_IsString(v));
    return strtoull(v->valuestring, NULL, 10);
}

static cJSON *query(query_t *q)
{
    int status = 0;
    char *text = ogs_ue_events_query(argument, q, &status);
    ogs_assert(status == 200 && text);
    cJSON *root = cJSON_Parse(text);
    free(text);
    ogs_assert(root && cJSON_IsArray(field(root, "items")));
    uint64_t previous = q->paging ? strtoull(q->cursor, NULL, 10) : 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, field(root, "items")) {
        uint64_t seq = number(e, "seq");
        ogs_assert(seq > previous);
        previous = seq;
        ogs_assert(cJSON_IsString(field(e, "supi")));
    }
    return root;
}

static void *reader(void *unused)
{
    query_t q = {0};
    while (!atomic_load(&stopped)) {
        cJSON *root = query(&q);
        ogs_assert(cJSON_GetArraySize(field(root, "items")) <= 1000);
        cJSON_Delete(root);
        atomic_fetch_add(&reads, 1);
        usleep(20000);
    }
    return NULL;
}

static size_t live_bytes(void)
{
#ifdef HAVE_MALLINFO2
    struct mallinfo2 info = mallinfo2();
    return info.uordblks + info.hblkhd;
#else
    return 0; /* mallinfo2 is optional; retention bounds are always checked. */
#endif
}

static size_t rss_bytes(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    unsigned long size = 0, rss = 0;
    ogs_assert(f && fscanf(f, "%lu %lu", &size, &rss) == 2);
    fclose(f);
    return rss * (size_t)sysconf(_SC_PAGESIZE);
}

static uint64_t verify_retained(size_t capacity)
{
    query_t q = {.paging = true, .cursor = "0"};
    size_t total = 0;
    unsigned per_user[USERS] = {0};
    uint64_t latest = 0;
    for (;;) {
        cJSON *root = query(&q);
        ogs_assert((size_t)field(root, "capacity")->valuedouble == capacity);
        ogs_assert(field(root, "per_ue")->valueint == 512);
        const cJSON *e;
        cJSON_ArrayForEach(e, field(root, "items")) {
            const char *supi = field(e, "supi")->valuestring;
            ogs_assert(strlen(supi) >= 6);
            unsigned user = strtoul(supi + strlen(supi) - 6, NULL, 10);
            ogs_assert(user < USERS && ++per_user[user] <= 512);
            total++;
        }
        latest = number(root, "latest_seq");
        bool more = cJSON_IsTrue(field(root, "has_more"));
        snprintf(q.cursor, sizeof(q.cursor), "%s", field(root, "next_seq")->valuestring);
        cJSON_Delete(root);
        if (!more) break;
    }
    ogs_assert(total == capacity && latest > capacity);
    printf("RETAINED capacity=%zu actual=%zu latest=%" PRIu64 "\n", capacity, total, latest);
    return latest;
}

static void run(size_t capacity, uint64_t cycles)
{
    ogs_assert(ogs_ue_events_init("AMF", capacity, 512));
    atomic_store(&stopped, false);
    atomic_store(&reads, 0);
    pthread_t thread;
    ogs_assert(!pthread_create(&thread, NULL, reader, NULL));
    size_t baseline = 0, maximum = 0;
    for (uint64_t i = 0; i < cycles; i++) {
        ogs_ue_event_t e = {.trace_id = ogs_ue_events_new_trace(), .has_gnb = true,
            .gnb_id = 3584, .gnb_bits = 28, .has_location = true, .nci = (3584ULL << 8) | 78};
        char supi[64];
        snprintf(supi, sizeof(supi), "imsi-001010000%06u", (unsigned)(i % USERS));
        snprintf(e.protocol, sizeof(e.protocol), "TEST");
        snprintf(e.name, sizeof(e.name), "InitialUEMessage");
        snprintf(e.result, sizeof(e.result), "received");
        ogs_assert(ogs_ue_events_record(&e));
        snprintf(e.name, sizeof(e.name), "RANAccess"); e.action = true;
        ogs_assert(ogs_ue_events_record(&e));
        ogs_ue_events_bind(e.trace_id, supi);
        snprintf(e.supi, sizeof(e.supi), "%s", supi);
        snprintf(e.name, sizeof(e.name), "Registration");
        snprintf(e.result, sizeof(e.result), "completed");
        ogs_assert(ogs_ue_events_record(&e));
        e.psi = (i / USERS) % 2 ? 2 : 5;
        snprintf(e.name, sizeof(e.name), "SessionModification");
        ogs_assert(ogs_ue_events_record(&e));
        if ((i + 1) % 50000 == 0 || i + 1 == cycles) {
            size_t live = live_bytes();
            if (!baseline) baseline = live;
            if (live > maximum) maximum = live;
#if !defined(__SANITIZE_ADDRESS__) && !__has_feature(address_sanitizer)
            ogs_assert(live <= baseline + 32 * 1024 * 1024);
#endif
            printf("CHECKPOINT capacity=%zu cycles=%" PRIu64 " live=%zu rss=%zu reads=%" PRIuFAST64 "\n",
                    capacity, i + 1, live, rss_bytes(), atomic_load(&reads));
        }
    }
    atomic_store(&stopped, true);
    ogs_assert(!pthread_join(thread, NULL));
    uint64_t latest = verify_retained(capacity);
    /* A single noisy user must not defeat the per-SUPI retention bound. */
    uint64_t trace = ogs_ue_events_new_trace();
    for (int i = 0; i < 10000; i++) {
        ogs_ue_event_t e = {.trace_id = trace, .psi = i % 2 ? 2 : 5};
        snprintf(e.supi, sizeof(e.supi), "hot-user");
        snprintf(e.name, sizeof(e.name), "HotUserModification");
        ogs_assert(ogs_ue_events_record(&e));
    }
    query_t q = {.supi = "hot-user"};
    cJSON *root = query(&q);
    ogs_assert(cJSON_GetArraySize(field(root, "items")) == 512);
    unsigned by_psi[16] = {0};
    const cJSON *event;
    cJSON_ArrayForEach(event, field(root, "items")) {
        unsigned psi = field(event, "psi")->valueint;
        ogs_assert(psi == 2 || psi == 5);
        by_psi[psi]++;
    }
    ogs_assert(by_psi[2] == 256 && by_psi[5] == 256);
    ogs_assert(number(root, "latest_seq") == latest + 10000);
    cJSON_Delete(root);
    printf("SOAK_PASS capacity=%zu cycles=%" PRIu64 " writes=%" PRIu64 " heap_baseline=%zu heap_max=%zu final_live=%zu rss=%zu\n",
            capacity, cycles, latest + 10000, baseline, maximum, live_bytes(), rss_bytes());
    ogs_ue_events_final();
}

static uint64_t parse_cycles(const char *text, uint64_t minimum)
{
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    ogs_assert(text[0] != '-' && end != text && !*end && errno != ERANGE);
    ogs_assert(value >= minimum && value <= 100000000);
    return value;
}

int main(int argc, char **argv)
{
    ogs_assert(argc <= 3);
    setvbuf(stdout, NULL, _IOLBF, 0);
    ogs_core_initialize();
    run(65536, argc > 1 ? parse_cycles(argv[1], 65536 / 5 + 1) : 1000000);
    run(262144, argc > 2 ? parse_cycles(argv[2], 262144 / 5 + 1) : 300000);
    ogs_core_terminate();
    talloc_disable_null_tracking();
    return 0;
}
