/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "ue-trace.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <unistd.h>
#include <endian.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define MAGIC 0x58435452u
#define VERSION 1u
#define MAX_SLOTS 256u
#define BYTE_RATE (1024u * 1024u)
typedef struct {
    uint64_t seq, timestamp_us;
    uint32_t length;
    uint8_t kind, security;
    uint8_t data[OGS_TRACE_MAX_PDU];
} slot_t;
typedef struct {
    uint32_t magic, version, slots, slot_size;
    char boot[36], nf[4];
    _Atomic uint64_t head, tail, lease_ns;
    _Atomic uint64_t captured, full, oversize, limited, wrong_thread;
    slot_t data[];
} ring_t;
typedef struct { ring_t *ring; size_t size; int fd; dev_t dev; ino_t ino; } mapping_t;
static mapping_t producer;
static _Atomic long owner;
static _Thread_local long thread_id;
static uint64_t last_ns;
static double byte_tokens, record_tokens;
static slot_t *pending;

static uint64_t now_ns(clockid_t clock_id)
{
    struct timespec t;
    if (clock_gettime(clock_id, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

int ogs_ue_trace_active(void)
{
    ring_t *r = producer.ring;
    if (!r) return 0;
    uint64_t lease = atomic_load_explicit(&r->lease_ns, memory_order_acquire);
    return lease && now_ns(CLOCK_MONOTONIC) < lease;
}

void ogs_ue_trace_close(void)
{
    if (producer.ring) munmap(producer.ring, producer.size);
    if (producer.ring) close(producer.fd);
    memset(&producer, 0, sizeof(producer));
    pending = NULL;
    atomic_store(&owner, 0);
}

int ogs_ue_trace_open(const char *nf, const char *boot)
{
    const char *dir = getenv("XCN_TRACE_DIR");
    const char *on = getenv("XCN_TRACE_ENABLED");
    char path[PATH_MAX], temp[PATH_MAX];
    unsigned slots;
    int fd;
    ring_t *r;
    size_t size;
    ogs_ue_trace_close();
    if (!on || strcmp(on, "1") || !dir || !boot || strlen(boot) != 36) return 0;
    if (!strcmp(nf, "AMF")) slots = 256;
    else if (!strcmp(nf, "SMF")) slots = 128;
    else return 0;
    if (snprintf(path, sizeof(path), "%s/%s.ring", dir, nf) >= (int)sizeof(path) ||
            snprintf(temp, sizeof(temp), "%s/.%s.%ld", dir, nf, (long)getpid()) >= (int)sizeof(temp)) return 0;
    size = sizeof(*r) + slots * sizeof(slot_t);
    /* A crashed startup may leave this private, unpublished file behind. */
    unlink(temp);
    fd = open(temp, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    /* Reserve tmpfs pages before publishing; ftruncate alone permits SIGBUS. */
    if (posix_fallocate(fd, 0, (off_t)size)) goto fail;
    r = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) goto fail;
    memset(r, 0, size);
    atomic_init(&r->head, 0); atomic_init(&r->tail, 0); atomic_init(&r->lease_ns, 0);
    atomic_init(&r->captured, 0); atomic_init(&r->full, 0); atomic_init(&r->oversize, 0);
    atomic_init(&r->limited, 0); atomic_init(&r->wrong_thread, 0);
    if (!atomic_is_lock_free(&r->head) || !atomic_is_lock_free(&owner)) {
        munmap(r, size); goto fail;
    }
    r->magic = MAGIC; r->version = VERSION; r->slots = slots; r->slot_size = sizeof(slot_t);
    memcpy(r->boot, boot, 36); memcpy(r->nf, nf, 3);
    if (rename(temp, path)) { munmap(r, size); goto fail; }
    producer = (mapping_t){.ring = r, .size = size, .fd = fd};
    last_ns = now_ns(CLOCK_MONOTONIC);
    byte_tokens = BYTE_RATE; record_tokens = 1000;
    return 1;
fail:
    close(fd); unlink(temp); return 0;
}

void *ogs_ue_trace_begin(unsigned kind, unsigned security, const void *data, size_t len)
{
    ring_t *r = producer.ring;
    uint64_t now, head, tail;
    long tid, expected = 0;
    slot_t *s;
    if (!r) return NULL;
    uint64_t lease = atomic_load_explicit(&r->lease_ns, memory_order_acquire);
    if (!lease) return NULL;
    now = now_ns(CLOCK_MONOTONIC);
    if (!now || now >= lease) {
        atomic_compare_exchange_strong_explicit(&r->lease_ns, &lease, 0,
                memory_order_relaxed, memory_order_relaxed);
        return NULL;
    }
    /* Thread ownership protects SPSC even if a future caller uses another thread. */
    if (!thread_id) thread_id = (long)syscall(SYS_gettid);
    tid = thread_id;
    if (!atomic_load_explicit(&owner, memory_order_relaxed))
        atomic_compare_exchange_strong_explicit(&owner, &expected, tid, memory_order_relaxed, memory_order_relaxed);
    if (atomic_load_explicit(&owner, memory_order_relaxed) != tid) {
        atomic_fetch_add_explicit(&r->wrong_thread, 1, memory_order_relaxed); return NULL;
    }
    if (pending || !data || !len || kind < 1 || kind > 3 || (security & OGS_TRACE_UNAVAILABLE)) return NULL;
    if (len > OGS_TRACE_MAX_PDU) { atomic_fetch_add_explicit(&r->oversize, 1, memory_order_relaxed); return NULL; }
    double elapsed = (now - last_ns) / 1e9;
    byte_tokens += elapsed * BYTE_RATE; record_tokens += elapsed * 1000;
    if (byte_tokens > BYTE_RATE) byte_tokens = BYTE_RATE;
    if (record_tokens > 1000) record_tokens = 1000;
    last_ns = now;
    if (byte_tokens < len || record_tokens < 1) {
        atomic_fetch_add_explicit(&r->limited, 1, memory_order_relaxed); return NULL;
    }
    head = atomic_load_explicit(&r->head, memory_order_relaxed);
    tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (head < tail || head - tail >= r->slots || head == UINT64_MAX) {
        atomic_fetch_add_explicit(&r->full, 1, memory_order_relaxed); return NULL;
    }
    byte_tokens -= len; record_tokens -= 1;
    s = &r->data[head % r->slots];
    s->length = (uint32_t)len; s->kind = kind; s->security = security;
    s->timestamp_us = now_ns(CLOCK_REALTIME) / 1000;
    memcpy(s->data, data, len);
    pending = s;
    return s;
}

void ogs_ue_trace_commit(void *ticket, uint64_t seq)
{
    ring_t *r = producer.ring;
    if (!ticket || ticket != pending || !r) return;
    if (seq) {
        pending->seq = seq;
        atomic_fetch_add_explicit(&r->head, 1, memory_order_release);
        atomic_fetch_add_explicit(&r->captured, 1, memory_order_relaxed);
    }
    pending = NULL;
}

void ogs_ue_trace_record(uint64_t seq, unsigned kind, unsigned security, const void *data, size_t len)
{
    if (seq) ogs_ue_trace_commit(ogs_ue_trace_begin(kind, security, data, len), seq);
}

void *xcn_trace_reader_open(const char *path)
{
    struct stat st;
    mapping_t *m;
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    if (flock(fd, LOCK_EX | LOCK_NB) || fstat(fd, &st) || !S_ISREG(st.st_mode) ||
            st.st_size < (off_t)sizeof(ring_t) || st.st_size > (off_t)(sizeof(ring_t) + MAX_SLOTS * sizeof(slot_t))) goto fail;
    ring_t *r = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (r == MAP_FAILED) goto fail;
    if (r->magic != MAGIC || r->version != VERSION || !r->slots || r->slots > MAX_SLOTS ||
            r->slot_size != sizeof(slot_t) || st.st_size != (off_t)(sizeof(*r) + r->slots * sizeof(slot_t)) ||
            !atomic_is_lock_free(&r->head)) { munmap(r, st.st_size); goto fail; }
    m = calloc(1, sizeof(*m));
    if (!m) { munmap(r, st.st_size); goto fail; }
    *m = (mapping_t){.ring = r, .fd = fd, .size = st.st_size, .dev = st.st_dev, .ino = st.st_ino};
    return m;
fail:
    close(fd); return NULL;
}

void xcn_trace_reader_lease(void *reader, int enabled)
{
    mapping_t *m = reader;
    if (m) atomic_store_explicit(&m->ring->lease_ns,
            enabled ? now_ns(CLOCK_MONOTONIC) + 6000000000ull : 0, memory_order_release);
}

void xcn_trace_reader_close(void *reader)
{
    mapping_t *m = reader;
    if (!m) return;
    xcn_trace_reader_lease(m, 0);
    munmap(m->ring, m->size); close(m->fd); free(m);
}

int xcn_trace_reader_current(void *reader, const char *path)
{
    mapping_t *m = reader;
    struct stat st;
    return m && !lstat(path, &st) && st.st_dev == m->dev && st.st_ino == m->ino;
}

int xcn_trace_reader_read(void *reader, void *out, size_t capacity)
{
    mapping_t *m = reader;
    if (!m || capacity < OGS_TRACE_WIRE_HEADER + OGS_TRACE_MAX_PDU) return -1;
    ring_t *r = m->ring;
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    if (head < tail || head - tail > r->slots) return -1;
    if (head == tail) return 0;
    slot_t *s = &r->data[tail % r->slots];
    if (!s->length || s->length > OGS_TRACE_MAX_PDU || s->kind < 1 || s->kind > 3) return -1;
    unsigned char *p = out;
    uint64_t v64 = htobe64(s->seq);
    uint32_t v32 = htobe32(s->length);
    memcpy(p, "XTR1", 4); p[4] = s->kind; p[5] = s->security; p[6] = p[7] = 0;
    memcpy(p + 8, &v64, 8); v64 = htobe64(s->timestamp_us);
    memcpy(p + 16, &v64, 8); memcpy(p + 24, &v32, 4);
    memcpy(p + 28, r->boot, 36); memcpy(p + 64, r->nf, 4);
    memcpy(p + OGS_TRACE_WIRE_HEADER, s->data, s->length);
    int length = OGS_TRACE_WIRE_HEADER + s->length;
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    return length;
}

void xcn_trace_reader_stats(void *reader, uint64_t out[5])
{
    mapping_t *m = reader;
    if (!m) { memset(out, 0, 5 * sizeof(*out)); return; }
    ring_t *r = m->ring;
    out[0] = atomic_load_explicit(&r->captured, memory_order_relaxed);
    out[1] = atomic_load_explicit(&r->full, memory_order_relaxed);
    out[2] = atomic_load_explicit(&r->oversize, memory_order_relaxed);
    out[3] = atomic_load_explicit(&r->limited, memory_order_relaxed);
    out[4] = atomic_load_explicit(&r->wrong_thread, memory_order_relaxed);
}
