/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ue-events.h"
#include "sbi/openapi/external/cJSON.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdlib.h>

#define NONE (-1)
#define MAX_CAPACITY 262144
#define MAX_QUERY 1000
typedef struct {
    ogs_ue_event_t value;
    int prev, next, free_next, group[2], before[2], after[2];
} slot_t;
typedef struct {
    char key[96];
    int head, tail, count, next;
} group_t;
static struct {
    slot_t *slots;
    group_t *groups;
    int *hash;
    size_t capacity, per_ue;
    int head, tail, free_slot, free_group;
    uint64_t seq, trace_seq, evicted, last_evicted;
    int64_t started, started_monotonic;
    char nf[8], boot[OGS_UUID_FORMATTED_LENGTH + 1];
} store;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static unsigned hash_key(const char *key)
{
    unsigned h = 2166136261u;
    while (*key) { h ^= (unsigned char)*key++; h *= 16777619u; }
    return h % (store.capacity * 2);
}

static int group_get(const char *key, bool create)
{
    unsigned h = hash_key(key);
    int n;
    for (n = store.hash[h]; n != NONE; n = store.groups[n].next)
        if (!strcmp(store.groups[n].key, key)) return n;
    if (!create || store.free_group == NONE) return NONE;
    n = store.free_group;
    store.free_group = store.groups[n].next;
    group_t *g = &store.groups[n];
    memset(g, 0, sizeof(*g));
    snprintf(g->key, sizeof(g->key), "%s", key);
    g->head = g->tail = NONE;
    g->next = store.hash[h]; store.hash[h] = n;
    return n;
}

static void unlink_group(int index, int axis)
{
    slot_t *s = &store.slots[index];
    int id = s->group[axis];
    group_t *g = &store.groups[id];
    if (s->before[axis] != NONE) store.slots[s->before[axis]].after[axis] = s->after[axis];
    else g->head = s->after[axis];
    if (s->after[axis] != NONE) store.slots[s->after[axis]].before[axis] = s->before[axis];
    else g->tail = s->before[axis];
    if (--g->count == 0) {
        int *p = &store.hash[hash_key(g->key)];
        while (*p != id) p = &store.groups[*p].next;
        *p = g->next;
        g->next = store.free_group; store.free_group = id;
    }
}

static void link_group(int index, int axis, int id)
{
    slot_t *s = &store.slots[index];
    group_t *g = &store.groups[id];
    s->group[axis] = id;
    s->before[axis] = g->tail; s->after[axis] = NONE;
    if (g->tail != NONE) store.slots[g->tail].after[axis] = index;
    else g->head = index;
    g->tail = index; g->count++;
}

/* Binding an older anonymous trace must preserve chronology in the SUPI index. */
static void link_owner(int index, int id)
{
    slot_t *s = &store.slots[index];
    group_t *g = &store.groups[id];
    int before = g->tail;
    while (before != NONE && store.slots[before].value.seq > s->value.seq)
        before = store.slots[before].before[0];
    if (before == g->tail) { link_group(index, 0, id); return; }
    int after = before == NONE ? g->head : store.slots[before].after[0];
    s->group[0] = id; s->before[0] = before; s->after[0] = after;
    if (before != NONE) store.slots[before].after[0] = index; else g->head = index;
    if (after != NONE) store.slots[after].before[0] = index; else g->tail = index;
    g->count++;
}

static int step(int index, int axis, bool reverse)
{
    if (axis < 0) return reverse ? store.slots[index].prev : store.slots[index].next;
    return reverse ? store.slots[index].before[axis] : store.slots[index].after[axis];
}

static void evict(int index)
{
    slot_t *s = &store.slots[index];
    if (s->prev != NONE) store.slots[s->prev].next = s->next;
    else store.head = s->next;
    if (s->next != NONE) store.slots[s->next].prev = s->prev;
    else store.tail = s->prev;
    unlink_group(index, 0); unlink_group(index, 1);
    store.evicted++;
    if (s->value.seq > store.last_evicted) store.last_evicted = s->value.seq;
    s->free_next = store.free_slot; store.free_slot = index;
}

bool ogs_ue_events_init(const char *nf, size_t capacity, size_t per_ue)
{
    ogs_uuid_t uuid;
    ogs_ue_events_final();
    pthread_mutex_lock(&lock);
    snprintf(store.nf, sizeof(store.nf), "%s", nf);
    ogs_uuid_get(&uuid); ogs_uuid_format(store.boot, &uuid);
    store.started = ogs_time_now();
    store.started_monotonic = ogs_get_monotonic_time();
    store.head = store.tail = store.free_slot = store.free_group = NONE;
    if (!capacity || capacity > MAX_CAPACITY || !per_ue || per_ue > capacity) goto fail;
    store.slots = calloc(capacity, sizeof(*store.slots));
    store.groups = calloc(capacity * 2, sizeof(*store.groups));
    store.hash = malloc(capacity * 2 * sizeof(*store.hash));
    if (!store.slots || !store.groups || !store.hash) goto fail;
    store.capacity = capacity; store.per_ue = per_ue;
    for (size_t i = 0; i < capacity; i++) store.slots[i].free_next = i + 1 < capacity ? i + 1 : NONE;
    for (size_t i = 0; i < capacity * 2; i++) {
        store.hash[i] = NONE;
        store.groups[i].next = i + 1 < capacity * 2 ? i + 1 : NONE;
    }
    store.free_slot = store.free_group = 0;
    pthread_mutex_unlock(&lock);
    return true;
fail:
    free(store.slots); free(store.groups); free(store.hash);
    store.slots = NULL; store.groups = NULL; store.hash = NULL;
    pthread_mutex_unlock(&lock);
    return false;
}

static bool unsigned_value(const char *s, uint64_t *value)
{
    char *end;
    if (!s || !*s) return false;
    for (const char *p = s; *p; p++) if (*p < '0' || *p > '9') return false;
    errno = 0; *value = strtoull(s, &end, 10);
    return !errno && !*end;
}

void ogs_ue_events_open(const char *nf)
{
    const char *enabled = getenv("OPEN5GS_UE_EVENTS_ENABLED");
    const char *cap = getenv("OPEN5GS_UE_EVENTS_CAPACITY");
    const char *per = getenv("OPEN5GS_UE_EVENTS_PER_UE");
    uint64_t capacity = 65536, per_ue = 512;
    bool valid = (!cap || unsigned_value(cap, &capacity)) &&
        (!per || unsigned_value(per, &per_ue));
    bool on = !enabled || !strcmp(enabled, "true") || !strcmp(enabled, "1");
    if (!valid || capacity > MAX_CAPACITY || per_ue > capacity) on = false;
    if (!ogs_ue_events_init(nf, on ? capacity : 0, per_ue))
        ogs_warn("%s UE event history disabled (configuration or allocation)", nf);
}

void ogs_ue_events_final(void)
{
    pthread_mutex_lock(&lock);
    free(store.slots); free(store.groups); free(store.hash);
    memset(&store, 0, sizeof(store));
    pthread_mutex_unlock(&lock);
}

bool ogs_ue_events_enabled(void) { return store.slots != NULL; }

uint64_t ogs_ue_events_new_trace(void)
{
    uint64_t n = 0;
    pthread_mutex_lock(&lock);
    if (store.slots && store.trace_seq != UINT64_MAX) n = ++store.trace_seq;
    pthread_mutex_unlock(&lock);
    return n;
}

uint64_t ogs_ue_events_record(const ogs_ue_event_t *event)
{
    uint64_t seq = 0;
    char owner[96], trace[96];
    pthread_mutex_lock(&lock);
    if (!store.slots || !event || !event->trace_id || store.seq == UINT64_MAX) goto done;
    if (store.free_slot == NONE) evict(store.head);
    int index = store.free_slot;
    slot_t *s = &store.slots[index];
    store.free_slot = s->free_next;
    memset(s, 0, sizeof(*s)); s->value = *event;
    /* Hooks use fixed zero-initialized snapshots; enforce string boundaries too. */
#define TERMINATE(k) s->value.k[sizeof(s->value.k)-1] = '\0'
    TERMINATE(supi); TERMINATE(protocol); TERMINATE(direction); TERMINATE(name);
    TERMINATE(result); TERMINATE(cause); TERMINATE(info); TERMINATE(plmn);
    TERMINATE(peer); TERMINATE(dnn); TERMINATE(cell_plmn);
#undef TERMINATE
    seq = s->value.seq = ++store.seq;
    if (!s->value.timestamp) s->value.timestamp = ogs_time_now();
    if (!s->value.monotonic) s->value.monotonic = ogs_get_monotonic_time();
    snprintf(trace, sizeof(trace), "t:%" PRIu64, event->trace_id);
    int existing = group_get(trace, false);
    if (!s->value.supi[0] && existing != NONE && store.groups[existing].tail != NONE)
        snprintf(s->value.supi, sizeof(s->value.supi), "%s",
                store.slots[store.groups[existing].tail].value.supi);
    snprintf(owner, sizeof(owner), s->value.supi[0] ? "u:%s" : "u:#%s", s->value.supi[0] ? s->value.supi : trace);
    int t = group_get(trace, true), u = group_get(owner, true);
    /* Two index pools are sized for two groups per retained event. */
    link_group(index, 0, u); link_group(index, 1, t);
    s->prev = store.tail; s->next = NONE;
    if (store.tail != NONE) store.slots[store.tail].next = index;
    else store.head = index;
    store.tail = index;
    while ((size_t)store.groups[u].count > store.per_ue) evict(store.groups[u].head);
done:
    pthread_mutex_unlock(&lock);
    return seq;
}

void ogs_ue_events_bind(uint64_t trace_id, const char *supi)
{
    char trace[96], owner[96];
    bool changed = false;
    if (!trace_id || !supi || !*supi || strlen(supi) >= sizeof(((ogs_ue_event_t *)0)->supi)) return;
    snprintf(trace, sizeof(trace), "t:%" PRIu64, trace_id);
    snprintf(owner, sizeof(owner), "u:%s", supi);
    pthread_mutex_lock(&lock);
    if (!store.slots) goto done;
    int t = group_get(trace, false);
    if (t == NONE) goto done;
    if (store.slots[store.groups[t].head].value.supi[0]) goto done;
    for (int n = store.groups[t].head; n != NONE;) {
        slot_t *s = &store.slots[n];
        int next = s->after[1];
        if (!s->value.supi[0]) {
            unlink_group(n, 0);
            link_owner(n, group_get(owner, true));
            snprintf(s->value.supi, sizeof(s->value.supi), "%s", supi);
            changed = true;
        }
        n = next;
    }
    int u = group_get(owner, false);
    if (u != NONE) while ((size_t)store.groups[u].count > store.per_ue) evict(store.groups[u].head);
done:
    pthread_mutex_unlock(&lock);
    if (changed) {
        ogs_ue_event_t e = {.trace_id = trace_id};
        snprintf(e.supi, sizeof(e.supi), "%s", supi);
        snprintf(e.protocol, sizeof(e.protocol), "IDENTITY");
        snprintf(e.name, sizeof(e.name), "IdentityResolved");
        snprintf(e.result, sizeof(e.result), "bound");
        ogs_ue_events_record(&e);
    }
}

static void json_u64(cJSON *obj, const char *key, uint64_t value)
{
    char text[24]; snprintf(text, sizeof(text), "%" PRIu64, value);
    cJSON_AddStringToObject(obj, key, text);
}

char *ogs_ue_events_query(ogs_query_arg_t arg, void *request, int *status)
{
    const char *supi = arg(request, "supi"), *id = arg(request, "trace_id");
    const char *after = arg(request, "after"), *boot = arg(request, "boot_id");
    const char *since = arg(request, "since_us"), *lim = arg(request, "limit");
    const char *detail = arg(request, "detail");
    const char *trace_boot = arg(request, "trace_boot_id");
    uint64_t trace_id = 0, cursor = 0, since_us = 0, limit = 100;
    *status = 400;
    if ((supi && (!*supi || strlen(supi) >= 64)) || (supi && id) ||
        (id && (!unsigned_value(id, &trace_id) || !trace_id)) ||
        (after && !unsigned_value(after, &cursor)) ||
        (since && (!unsigned_value(since, &since_us) || since_us > INT64_MAX)) ||
        (lim && (!unsigned_value(lim, &limit) || !limit || limit > MAX_QUERY)) ||
        (detail && strcmp(detail, "0") && strcmp(detail, "1")))
        return strdup("{\"error\":\"invalid UE event query\"}");
    ogs_ue_event_t *snapshot = calloc(limit, sizeof(*snapshot));
    cJSON *root = cJSON_CreateObject(), *items = cJSON_CreateArray();
    if (!snapshot || !root || !items) {
        free(snapshot); cJSON_Delete(root); cJSON_Delete(items);
        *status = 500; return strdup("{\"error\":\"out of memory\"}");
    }
    cJSON_AddItemToObject(root, "items", items);
    size_t count = 0;
    bool reset, more = false, steps = detail && !strcmp(detail, "1");
    uint64_t next;
    pthread_mutex_lock(&lock);
    reset = boot && strcmp(boot, store.boot);
    bool incremental = after && !reset;
    next = incremental ? cursor : store.seq;
    int axis = -1, tail = store.slots ? store.tail : NONE;
    if (store.slots && (supi || id)) {
        char key[96];
        if (supi) snprintf(key, sizeof(key), "u:%s", supi);
        else snprintf(key, sizeof(key), "t:%" PRIu64, trace_id);
        int group = group_get(key, false);
        tail = group == NONE ? NONE : store.groups[group].tail;
        axis = supi ? 0 : 1;
    }
    int start = tail;
    if (incremental) {
        start = NONE;
        for (int n = tail; n != NONE && store.slots[n].value.seq > cursor; n = step(n, axis, true)) start = n;
    }
    for (int n = start; n != NONE; n = step(n, axis, !incremental)) {
        const ogs_ue_event_t *e = &store.slots[n].value;
        if (incremental && e->seq <= cursor) continue;
        bool matches = (!supi || !strcmp(supi, e->supi)) && (!id || e->trace_id == trace_id) &&
            (!trace_boot || !strcmp(trace_boot, store.boot)) &&
            (!since || e->timestamp >= (int64_t)since_us) &&
            (steps || e->action || !strcmp(e->protocol, "IDENTITY"));
        if (matches && count == limit) { more = true; break; }
        if (matches) snapshot[count++] = *e;
        if (incremental) next = e->seq;
    }
    if (incremental && !more) next = store.seq;
    cJSON_AddStringToObject(root, "nf", store.nf);
    cJSON_AddStringToObject(root, "boot_id", store.boot);
    cJSON_AddBoolToObject(root, "enabled", store.slots != NULL);
    cJSON_AddNumberToObject(root, "started_at_us", store.started);
    cJSON_AddNumberToObject(root, "capacity", store.capacity);
    cJSON_AddNumberToObject(root, "per_ue", store.per_ue);
    json_u64(root, "oldest_seq", store.slots && store.head != NONE ? store.slots[store.head].value.seq : 0);
    json_u64(root, "latest_seq", store.seq); json_u64(root, "next_seq", next);
    json_u64(root, "overwritten_total", store.evicted);
    cJSON_AddBoolToObject(root, "reset", reset);
    cJSON_AddBoolToObject(root, "gap", incremental && cursor < store.last_evicted);
    cJSON_AddBoolToObject(root, "has_more", more);
    char nf[8], epoch[sizeof(store.boot)];
    int64_t base_time = store.started, base_monotonic = store.started_monotonic;
    memcpy(nf, store.nf, sizeof(nf)); memcpy(epoch, store.boot, sizeof(epoch));
    pthread_mutex_unlock(&lock);
    for (size_t i = 0; i < count; i++) {
        const ogs_ue_event_t *e = &snapshot[incremental ? i : count-1-i];
        cJSON *o = cJSON_CreateObject();
        if (!o) { *status = 500; goto done; }
        cJSON_AddItemToArray(items, o);
        char event_id[96]; snprintf(event_id, sizeof(event_id), "%s/%s/%" PRIu64, nf, epoch, e->seq);
        cJSON_AddStringToObject(o, "event_id", event_id);
        json_u64(o, "seq", e->seq); json_u64(o, "trace_id", e->trace_id);
        json_u64(o, "parent_seq", e->parent_seq); json_u64(o, "flow_id", e->flow_id);
        cJSON_AddNumberToObject(o, "timestamp_us", e->timestamp);
        cJSON_AddNumberToObject(o, "order_timestamp_us", base_time + e->monotonic - base_monotonic);
        json_u64(o, "monotonic_us", e->monotonic);
        cJSON_AddBoolToObject(o, "action", e->action);
#define STR(k) cJSON_AddStringToObject(o, #k, e->k)
        STR(supi); STR(protocol); STR(direction); STR(name); STR(result); STR(cause); STR(info);
        STR(plmn); STR(cell_plmn); STR(peer); STR(dnn);
#undef STR
#define NUM(k) (e->k ? cJSON_AddNumberToObject(o, #k, e->k) : cJSON_AddNullToObject(o, #k))
        NUM(psi); NUM(pti); NUM(qfi); NUM(fiveqi);
#undef NUM
        cJSON *psis = cJSON_AddArrayToObject(o, "psis");
        for (size_t p = 0; psis && p < e->psi_count && p < sizeof(e->psis); p++)
            cJSON_AddItemToArray(psis, cJSON_CreateNumber(e->psis[p]));
        if (e->has_gnb) {
            cJSON_AddNumberToObject(o, "gnb_id", e->gnb_id); cJSON_AddNumberToObject(o, "gnb_bits", e->gnb_bits);
        } else { cJSON_AddNullToObject(o, "gnb_id"); cJSON_AddNullToObject(o, "gnb_bits"); }
        if (e->has_location) {
            json_u64(o, "nci", e->nci); cJSON_AddNumberToObject(o, "tac", e->tac);
        } else { cJSON_AddNullToObject(o, "nci"); cJSON_AddNullToObject(o, "tac"); }
        if (e->has_location && e->has_gnb && e->gnb_bits >= 22 && e->gnb_bits <= 32 && e->nci <= 0xfffffffffULL) {
            cJSON_AddNumberToObject(o, "cell_id", e->nci & ((1ULL << (36 - e->gnb_bits))-1));
            cJSON_AddNumberToObject(o, "gnb_id_from_nci", e->nci >> (36 - e->gnb_bits));
            cJSON_AddBoolToObject(o, "identity_consistent", (e->nci >> (36 - e->gnb_bits)) == e->gnb_id);
        } else { cJSON_AddNullToObject(o, "cell_id"); cJSON_AddNullToObject(o, "gnb_id_from_nci"); cJSON_AddNullToObject(o, "identity_consistent"); }
        if (e->upf_seid) json_u64(o, "upf_seid", e->upf_seid); else cJSON_AddNullToObject(o, "upf_seid");
        if (e->has_ngap) {
            json_u64(o, "amf_ngap_id", e->amf_ngap_id); json_u64(o, "ran_ngap_id", e->ran_ngap_id);
        } else { cJSON_AddNullToObject(o, "amf_ngap_id"); cJSON_AddNullToObject(o, "ran_ngap_id"); }
    }
    *status = 200;
done:
    free(snapshot);
    char *encoded = cJSON_PrintUnformatted(root);
    char *result = encoded ? strdup(encoded) : NULL;
    cJSON_free(encoded); cJSON_Delete(root);
    return result;
}
