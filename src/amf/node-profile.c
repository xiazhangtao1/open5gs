/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "node-profile.h"
#include <string.h>

#define NO_ENTRY UINT32_MAX

typedef struct {
    uint64_t timestamp;
    uint8_t random[XCN_AUTH_RANDOM_LEN];
    uint32_t hash_next;
    uint32_t previous;
    uint32_t next;
} profile_entry_t;

/* Fixed storage: no allocator or capacity failures on the NG Setup path. */
static struct {
    profile_entry_t entries[AMF_NODE_PROFILE_CAPACITY];
    uint32_t buckets[AMF_NODE_PROFILE_CAPACITY];
    uint32_t first, last, free_entry;
    size_t count;
    uint64_t last_cleanup;
    int cleanup_done;
} cache;

static uint32_t random_bucket(const uint8_t random[XCN_AUTH_RANDOM_LEN])
{
    uint32_t hash = 2166136261U;
    unsigned int i;
    for (i = 0; i < XCN_AUTH_RANDOM_LEN; i++)
        hash = (hash ^ random[i]) * 16777619U;
    return hash % AMF_NODE_PROFILE_CAPACITY;
}

void amf_node_profile_init(void)
{
    uint32_t i;
    memset(&cache, 0, sizeof(cache));
    for (i = 0; i < AMF_NODE_PROFILE_CAPACITY; i++) {
        cache.buckets[i] = NO_ENTRY;
        cache.entries[i].hash_next = i + 1;
    }
    cache.entries[AMF_NODE_PROFILE_CAPACITY - 1].hash_next = NO_ENTRY;
    cache.first = cache.last = NO_ENTRY;
    cache.free_entry = 0;
}

static void remove_entry(uint32_t index)
{
    profile_entry_t *entry = &cache.entries[index];
    uint32_t *link = &cache.buckets[random_bucket(entry->random)];

    while (*link != index)
        link = &cache.entries[*link].hash_next;
    *link = entry->hash_next;
    if (entry->previous != NO_ENTRY)
        cache.entries[entry->previous].next = entry->next;
    else
        cache.first = entry->next;
    if (entry->next != NO_ENTRY)
        cache.entries[entry->next].previous = entry->previous;
    else
        cache.last = entry->previous;
    entry->hash_next = cache.free_entry;
    cache.free_entry = index;
    cache.count--;
}

static void remove_expired(uint64_t now)
{
    uint32_t index, next;

    /* At most one bounded scan per wall-clock second, also after a rollback. */
    if (cache.cleanup_done && cache.last_cleanup == now)
        return;
    for (index = cache.first; index != NO_ENTRY; index = next) {
        profile_entry_t *entry = &cache.entries[index];
        next = entry->next;
        if (now > entry->timestamp &&
                now - entry->timestamp > AMF_NODE_PROFILE_WINDOW)
            remove_entry(index);
    }
    cache.last_cleanup = now;
    cache.cleanup_done = 1;
}

int amf_node_profile_check_payload(const xcn_auth_payload_t *payload,
        uint64_t now)
{
    uint64_t difference;
    uint32_t bucket, index;
    profile_entry_t *entry;

    if (!payload)
        return XCN_AUTH_ERR_PARAM;
    difference = now >= payload->timestamp ?
        now - payload->timestamp : payload->timestamp - now;
    if (difference > AMF_NODE_PROFILE_WINDOW)
        return XCN_AUTH_ERR_TIME;

    remove_expired(now);
    bucket = random_bucket(payload->random);
    for (index = cache.buckets[bucket]; index != NO_ENTRY;
            index = cache.entries[index].hash_next) {
        if (!memcmp(cache.entries[index].random, payload->random,
                    XCN_AUTH_RANDOM_LEN))
            return XCN_AUTH_ERR_REPLAY;
    }

    /* Capacity is a FIFO replacement policy, never an admission failure. */
    if (cache.count == AMF_NODE_PROFILE_CAPACITY)
        remove_entry(cache.first);
    index = cache.free_entry;
    entry = &cache.entries[index];
    cache.free_entry = entry->hash_next;
    entry->timestamp = payload->timestamp;
    memcpy(entry->random, payload->random, sizeof(entry->random));
    entry->hash_next = cache.buckets[bucket];
    cache.buckets[bucket] = index;
    entry->previous = cache.last;
    entry->next = NO_ENTRY;
    if (cache.last != NO_ENTRY)
        cache.entries[cache.last].next = index;
    else
        cache.first = index;
    cache.last = index;
    cache.count++;
    return XCN_AUTH_OK;
}

int amf_node_profile_check(const char *name, size_t length, uint64_t now)
{
    xcn_auth_payload_t payload;
    int result = amf_verify_ran_node_name(name, length, &payload);
    if (result == XCN_AUTH_OK)
        result = amf_node_profile_check_payload(&payload, now);
    return result;
}

size_t amf_node_profile_count(void)
{
    return cache.count;
}
