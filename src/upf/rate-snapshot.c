/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "rate-snapshot.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { FREE, WRITING, READY, READING };

int upf_snapshot_init(upf_snapshot_t *s, size_t sessions, size_t rules)
{
    unsigned i;
    struct timespec boot = {0};
    memset(s, 0, sizeof(*s));
    if (!sessions || sessions > 4096 || !rules || rules > 65536)
        return -1;
    s->max_sessions = sessions;
    s->max_rules = rules;
    clock_gettime(CLOCK_REALTIME, &boot);
    snprintf(s->boot_id, sizeof(s->boot_id), "%lld-%ld-%ld",
            (long long)boot.tv_sec, boot.tv_nsec, (long)getpid());
    for (i = 0; i < 2; i++) {
        s->frames[i].sessions = calloc(sessions, sizeof(upf_snapshot_session_t));
        s->frames[i].rules = calloc(rules, sizeof(upf_snapshot_rule_t));
        if (!s->frames[i].sessions || !s->frames[i].rules) {
            upf_snapshot_final(s);
            return -1;
        }
        /* Fault in anonymous pages now, outside the sampler's rule read lock. */
        {
            size_t j;
            volatile unsigned char *p = (void *)s->frames[i].sessions;
            for (j = 0; j < sessions * sizeof(upf_snapshot_session_t); j += 4096)
                p[j] = 0;
            p = (void *)s->frames[i].rules;
            for (j = 0; j < rules * sizeof(upf_snapshot_rule_t); j += 4096)
                p[j] = 0;
        }
    }
    return 0;
}

void upf_snapshot_final(upf_snapshot_t *s)
{
    unsigned i;
    for (i = 0; i < 2; i++) {
        free(s->frames[i].sessions);
        free(s->frames[i].rules);
    }
    memset(s, 0, sizeof(*s));
}

void upf_snapshot_subscribe(upf_snapshot_t *s, uint64_t now_us)
{
    __atomic_store_n(&s->lease_until_us, now_us + 6000000, __ATOMIC_RELEASE);
}

upf_snapshot_frame_t *upf_snapshot_begin(upf_snapshot_t *s, uint64_t now_us)
{
    unsigned i;
    if (now_us >= __atomic_load_n(&s->lease_until_us, __ATOMIC_ACQUIRE))
        return NULL;
    for (i = 0; i < 2; i++) {
        unsigned index = (s->next + i) % 2;
        upf_snapshot_frame_t *f = &s->frames[index];
        int expected = FREE;
        if (__atomic_compare_exchange_n(&f->state, &expected, WRITING,
                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ||
            (expected == READY && __atomic_compare_exchange_n(
                    &f->state, &expected, WRITING, false,
                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))) {
            s->next = (index + 1) % 2;
            f->session_count = f->rule_count = 0;
            f->truncated = false;
            f->version = ++s->version;
            f->monotonic_us = now_us;
            memcpy(f->boot_id, s->boot_id, sizeof(f->boot_id));
            return f;
        }
    }
    s->skipped++;
    return NULL;
}

void upf_snapshot_publish(upf_snapshot_t *s, upf_snapshot_frame_t *f)
{
    (void)s;
    __atomic_store_n(&f->state, READY, __ATOMIC_RELEASE);
}

upf_snapshot_frame_t *upf_snapshot_acquire(upf_snapshot_t *s)
{
    upf_snapshot_frame_t *latest = NULL;
    unsigned i;
    for (i = 0; i < 2; i++) {
        upf_snapshot_frame_t *f = &s->frames[i];
        int expected = READY;
        if (!__atomic_compare_exchange_n(&f->state, &expected, READING,
                    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;
        if (!latest || f->version > latest->version) {
            if (latest) upf_snapshot_release(latest);
            latest = f;
        } else upf_snapshot_release(f);
    }
    return latest;
}

void upf_snapshot_release(upf_snapshot_frame_t *f)
{
    __atomic_store_n(&f->state, READY, __ATOMIC_RELEASE);
}

static size_t emit(char *buf, size_t n, size_t cap, const char *fmt, ...)
{
    int written;
    va_list args;
    if (n >= cap) return cap;
    va_start(args, fmt);
    written = vsnprintf(buf + n, cap - n, fmt, args);
    va_end(args);
    return written < 0 || (size_t)written >= cap - n ? cap : n + written;
}

static size_t quote(char *buf, size_t n, size_t cap, const char *value)
{
    const unsigned char *p = (const unsigned char *)value;
    n = emit(buf, n, cap, "\"");
    for (; *p && n < cap; p++) {
        if (*p == '"' || *p == '\\') n = emit(buf, n, cap, "\\%c", *p);
        else if (*p < 32) n = emit(buf, n, cap, "\\u%04x", *p);
        else n = emit(buf, n, cap, "%c", *p);
    }
    return emit(buf, n, cap, "\"");
}

size_t upf_snapshot_json(const upf_snapshot_frame_t *f, char *buf, size_t cap)
{
    size_t n = 0, i, j;
    if (!f) return emit(buf, n, cap, "{\"ready\":false}\n");
    n = emit(buf, n, cap, "{\"ready\":true,\"schema\":1,\"boot_id\":\"%s\",\"version\":\"%"PRIu64
            "\",\"timestamp_us\":\"%"PRIu64"\",\"monotonic_us\":\"%"PRIu64
            "\",\"sample_us\":%"PRIu64",\"lock_wait_us\":%"PRIu64
            ",\"lock_hold_us\":%"PRIu64",\"truncated\":%s,\"sessions\":[",
            f->boot_id, f->version, f->timestamp_us, f->monotonic_us, f->sample_us,
            f->lock_wait_us, f->lock_hold_us,
            f->truncated ? "true" : "false");
    for (i = 0; i < f->session_count && n < cap; i++) {
        const upf_snapshot_session_t *s = &f->sessions[i];
        char ip4[INET_ADDRSTRLEN] = "", ip6[INET6_ADDRSTRLEN] = "";
        if (s->has_ipv4) inet_ntop(AF_INET, s->ipv4, ip4, sizeof(ip4));
        if (s->has_ipv6) inet_ntop(AF_INET6, s->ipv6, ip6, sizeof(ip6));
        n = emit(buf, n, cap, "%s{\"supi\":", i ? "," : "");
        n = quote(buf, n, cap, s->supi);
        n = emit(buf, n, cap, ",\"dnn\":");
        n = quote(buf, n, cap, s->dnn);
        n = emit(buf, n, cap, ",\"seid\":\"%"PRIu64"\",\"ipv4\":\"%s\","
                "\"ipv6\":\"%s\",\"ue_ip\":\"%s\",\"worker\":%u,"
                "\"uptime_seconds\":%"PRIu64",\"rules\":[",
                s->seid, ip4, ip6, s->has_ipv4 ? ip4 : ip6,
                s->worker, s->uptime_seconds);
        for (j = 0; j < s->rule_count && n < cap; j++) {
            const upf_snapshot_rule_t *r = &f->rules[s->first_rule + j];
            n = emit(buf, n, cap, "%s{\"pdr_id\":%u,\"qer_id\":%u,"
                    "\"generation\":%u,\"qfi\":%u,\"direction\":\"%s\","
                    "\"bps\":\"%"PRIu64"\",\"pps\":\"%"PRIu64"\","
                    "\"bytes\":\"%"PRIu64"\",\"packets\":\"%"PRIu64"\"}",
                    j ? "," : "", r->pdr_id, r->qer_id, r->generation, r->qfi,
                    r->direction == 0 ? "UL" : "DL",
                    r->bps, r->pps, r->octets, r->packets);
        }
        n = emit(buf, n, cap, "]}");
    }
    n = emit(buf, n, cap, "]}\n");
    if (n >= cap) return emit(buf, 0, cap, "{\"ready\":false,\"error\":\"snapshot too large\"}\n");
    return n;
}
