/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/upf/rate-snapshot.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static upf_snapshot_t cache;
static int done;

static void *producer(void *arg)
{
    unsigned i;
    (void)arg;
    for (i = 1; i <= 100000; i++) {
        upf_snapshot_frame_t *f = upf_snapshot_begin(&cache, 1);
        if (!f) continue;
        f->session_count = 1;
        f->rule_count = 1;
        memset(&f->sessions[0], 0, sizeof(f->sessions[0]));
        f->sessions[0].seid = f->version;
        f->sessions[0].rule_count = 1;
        memset(&f->rules[0], 0, sizeof(f->rules[0]));
        f->rules[0].octets = f->version;
        upf_snapshot_publish(&cache, f);
    }
    __atomic_store_n(&done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    pthread_t thread;
    upf_snapshot_frame_t *f, *held;
    char json[4096];
    unsigned reads = 0;
    assert(upf_snapshot_init(&cache, 4, 8) == 0);
    assert(!upf_snapshot_begin(&cache, 1));
    upf_snapshot_subscribe(&cache, 1);
    f = upf_snapshot_begin(&cache, 1);
    assert(f);
    f->session_count = 1;
    f->rule_count = 1;
    f->sessions[0].seid = UINT64_MAX;
    strcpy(f->sessions[0].dnn, "a\"b\\c\n");
    f->sessions[0].rule_count = 1;
    f->rules[0].octets = UINT64_MAX;
    f->rules[0].src_if = 3;
    f->rules[0].dst_if = 0;
    upf_snapshot_publish(&cache, f);
    held = upf_snapshot_acquire(&cache);
    assert(held == f);
    assert(upf_snapshot_json(f, json, sizeof(json)) < sizeof(json));
    assert(strstr(json, "\"18446744073709551615\""));
    assert(strstr(json, "\"src_if\":3,\"dst_if\":0"));
    assert(strstr(json, "a\\\"b\\\\c\\u000a"));
    f = upf_snapshot_begin(&cache, 1);
    assert(f && f != held);
    assert(!upf_snapshot_begin(&cache, 1)); /* Both owned: skip, don't wait. */
    upf_snapshot_publish(&cache, f);
    upf_snapshot_release(held);
    assert(!upf_snapshot_begin(&cache, 6000001)); /* Expired subscription. */
    /* Reset fixture before stressing producer/consumer ownership. */
    upf_snapshot_final(&cache);
    assert(upf_snapshot_init(&cache, 4, 8) == 0);
    upf_snapshot_subscribe(&cache, 1);
    assert(pthread_create(&thread, NULL, producer, NULL) == 0);
    do {
        f = upf_snapshot_acquire(&cache);
        if (!f) continue;
        assert(f->sessions[0].seid == f->version);
        assert(f->rules[0].octets == f->version);
        assert(upf_snapshot_json(f, json, sizeof(json)) < sizeof(json));
        assert(f->rules[0].octets == f->version);
        reads++;
        upf_snapshot_release(f);
    } while (!__atomic_load_n(&done, __ATOMIC_ACQUIRE));
    assert(pthread_join(thread, NULL) == 0);
    assert(reads);
    upf_snapshot_final(&cache);
    puts("snapshot ownership, expiry, precision and bounds: OK");
    return 0;
}
