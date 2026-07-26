/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "context.h"
#include "dataplane.h"
#include "gtp-path.h"
#include "n3-memif.h"
#include "n6-memif.h"

#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <pthread.h>
#include <sched.h>

#if HAVE_LIBMEMIF
#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#define UPF_MAX_WORKERS 16
#define UPF_EPOLL_MAX_EVENTS 32

typedef enum {
    UPF_WORK_N3,
    UPF_WORK_N6,
} upf_work_type_t;

typedef struct {
    upf_work_type_t type;
    size_t len;
    ogs_sockaddr_t from;
    const void *lease_data;
    void (*complete)(uint16_t qid, uint64_t sequence, uint32_t generation);
    uint64_t sequence;
    uint32_t generation;
    uint16_t qid;
    uint8_t data[];
} upf_work_t;

typedef struct {
    uint64_t start;
    uint16_t count;
} upf_work_batch_t;

typedef struct {
    uint8_t id;
    ogs_thread_t *thread;
    ogs_queue_t *queue;
    ogs_queue_t *free_queue;
    pthread_mutex_t submit_lock;
    bool submit_lock_initialized;
    void *task_pool;
    void *batch_pool;
    size_t task_size;
    uint64_t write_seq;
    uint64_t read_seq;
    uint64_t packets;
    uint64_t drops;
    uint64_t queue_full_batches;
    uint64_t queue_push_failures;
    uint64_t queue_high_water;
    uint64_t blocking_waits;
    uint64_t poll_hits;
    uint64_t poll_loops;
} upf_worker_t;

typedef enum {
    UPF_DISPATCH_N3,
    UPF_DISPATCH_N6,
    UPF_DISPATCH_MAX,
} upf_dispatch_type_t;

typedef struct {
    upf_dispatch_type_t type;
    const char *name;
    ogs_thread_t *thread;
#if HAVE_LIBMEMIF
    int epoll_fd;
    int wake_fd;
    uint8_t wake_token;
    uint64_t epoll_waits;
    uint64_t epoll_events;
    uint64_t epoll_errors;
    uint64_t epoll_wakeups;
#endif
} upf_dispatcher_t;

static upf_dispatcher_t dispatchers[UPF_DISPATCH_MAX];
static upf_worker_t workers[UPF_MAX_WORKERS];
static pthread_rwlock_t rule_lock;
static pthread_mutex_t report_lock;
static pthread_mutex_t legacy_io_lock;
static unsigned int owned_sessions[UPF_MAX_WORKERS];
static int stopping;
static int dispatcher_stopping;
static __thread uint8_t tls_worker_id;

static bool is_stopping(void)
{
    return __atomic_load_n(&stopping, __ATOMIC_ACQUIRE);
}

static bool is_dispatcher_stopping(void)
{
    return __atomic_load_n(&dispatcher_stopping, __ATOMIC_ACQUIRE);
}

static int read_allowed_cpus(int cpus[], int capacity)
{
    FILE *fp;
    char line[512];
    int count = 0;

    fp = fopen("/proc/thread-self/status", "r");
    if (!fp)
        return OGS_ERROR;
    while (fgets(line, sizeof(line), fp)) {
        char *p;

        if (strncmp(line, "Cpus_allowed_list:", 18))
            continue;
        p = line + 18;
        while (*p && count < capacity) {
            long first;
            long last;
            char *end;

            while (*p == ' ' || *p == '\t' || *p == ',')
                p++;
            if (*p == '\0' || *p == '\n')
                break;
            first = strtol(p, &end, 10);
            if (end == p)
                break;
            last = first;
            p = end;
            if (*p == '-') {
                last = strtol(p + 1, &end, 10);
                p = end;
            }
            while (first <= last && count < capacity)
                cpus[count++] = (int)first++;
        }
        break;
    }
    fclose(fp);
    return count;
}

static void pin_current_thread_when_ready(
        unsigned int index, const char *name)
{
    int cpus[CPU_SETSIZE];
    unsigned int required =
        upf_self()->dataplane.worker_count + 2; /* dispatcher + control */
    int retry;

    for (retry = 0; retry < 300 && !is_stopping(); retry++) {
        int count = read_allowed_cpus(cpus, CPU_SETSIZE);

        if (count >= required && cpus[0] != 0) {
            cpu_set_t selected;
            int cpu = cpus[index];

            CPU_ZERO(&selected);
            CPU_SET(cpu, &selected);
            if (sched_setaffinity(0, sizeof(selected), &selected) == 0) {
                ogs_info("Pinned %s to logical CPU %d [SCHED_OTHER]",
                        name, cpu);
                return;
            }
            ogs_warn("Cannot pin %s to logical CPU %d", name, cpu);
            return;
        }
        ogs_msleep(100);
    }
    ogs_warn("CPUManager cpuset not ready for %s; using pod cpuset", name);
}

static inline void worker_cpu_relax(void)
{
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

static int worker_pop(
        upf_worker_t *worker, upf_work_batch_t **batch, bool active)
{
    int32_t busy_poll_us = upf_self()->dataplane.worker_busy_poll_us;

    if (busy_poll_us < 0) {
        while (!is_stopping()) {
            worker->poll_loops++;
            if (ogs_queue_trypop(worker->queue, (void **)batch) == OGS_OK) {
                worker->poll_hits++;
                return OGS_OK;
            }
            worker_cpu_relax();
        }
        return OGS_ERROR;
    }

    if (active && busy_poll_us > 0) {
        ogs_time_t deadline =
            ogs_get_monotonic_time() + (ogs_time_t)busy_poll_us;

        do {
            worker->poll_loops++;
            if (ogs_queue_trypop(worker->queue, (void **)batch) == OGS_OK) {
                worker->poll_hits++;
                return OGS_OK;
            }
            worker_cpu_relax();
        } while (!is_stopping() &&
                ogs_get_monotonic_time() < deadline);
    }

    worker->blocking_waits++;
    return ogs_queue_pop(worker->queue, (void **)batch);
}

static void session_worker_main(void *data)
{
    upf_worker_t *worker = data;
    upf_work_batch_t *pending = NULL;
    bool active = false;

    tls_worker_id = worker->id;
    pin_current_thread_when_ready(worker->id, "session-worker");
    ogs_info("UPF session worker %u started", worker->id);
    while (!is_stopping()) {
        upf_work_batch_t *batches[UPF_DATAPLANE_MAX_BURST];
        unsigned int batch_count = 0;
        unsigned int packet_count = 0;
        unsigned int n6_count = 0;
        unsigned int i;
        int rv;

        if (pending) {
            batches[batch_count++] = pending;
            packet_count = pending->count;
            pending = NULL;
        } else {
            rv = worker_pop(worker, &batches[0], active);
            if (rv != OGS_OK || !batches[0])
                break;
            packet_count = batches[0]->count;
            batch_count = 1;
        }

        while (batch_count < UPF_DATAPLANE_MAX_BURST &&
                packet_count < UPF_DATAPLANE_MAX_BURST) {
            upf_work_batch_t *next = NULL;

            if (ogs_queue_trypop(worker->queue, (void **)&next) != OGS_OK)
                break;
            if (packet_count + next->count > UPF_DATAPLANE_MAX_BURST) {
                pending = next;
                break;
            }
            batches[batch_count++] = next;
            packet_count += next->count;
        }

        for (i = 0; i < batch_count; i++) {
            upf_work_batch_t *batch = batches[i];
            unsigned int j;

            for (j = 0; j < batch->count; j++) {
                uint64_t seq = batch->start + j;
                upf_work_t *work = (upf_work_t *)(
                        (uint8_t *)worker->task_pool +
                        (seq % upf_self()->dataplane.worker_queue_size) *
                        worker->task_size);
                if (work->type == UPF_WORK_N6)
                    n6_count++;
            }
        }

        upf_dataplane_read_lock();
        upf_n6_memif_tx_batch_begin();
        upf_n3_memif_tx_batch_begin(n6_count);
        for (i = 0; i < batch_count; i++) {
            upf_work_batch_t *batch = batches[i];
            unsigned int j;

            for (j = 0; j < batch->count; j++) {
                uint64_t seq = batch->start + j;
                upf_work_t *work = (upf_work_t *)(
                        (uint8_t *)worker->task_pool +
                        (seq % upf_self()->dataplane.worker_queue_size) *
                        worker->task_size);
                const void *packet_data =
                    work->complete ? work->lease_data : work->data;

                if (work->type == UPF_WORK_N3)
                    upf_gtp_handle_n3_data(
                            packet_data, work->len, &work->from);
                else
                    upf_gtp_handle_n6_data(packet_data, work->len);
                if (work->complete)
                    work->complete(work->qid,
                            work->sequence, work->generation);
            }
        }
        upf_n6_memif_tx_batch_flush();
        upf_n3_memif_tx_batch_flush();
        upf_dataplane_read_unlock();
        __atomic_fetch_add(&worker->packets,
                packet_count, __ATOMIC_RELAXED);
        active = true;
        for (i = 0; i < batch_count; i++) {
            upf_work_batch_t *batch = batches[i];

            __atomic_store_n(&worker->read_seq,
                    batch->start + batch->count, __ATOMIC_RELEASE);
            ogs_assert(ogs_queue_trypush(
                        worker->free_queue, batch) == OGS_OK);
        }
    }
    ogs_info("UPF session worker %u stopped [packets:%llu drops:%llu]",
            worker->id, (unsigned long long)worker->packets,
            (unsigned long long)worker->drops);
}

static void dispatcher_main(void *data)
{
    upf_dispatcher_t *dispatcher = data;
    ogs_time_t last_stats = ogs_get_monotonic_time();
    uint8_t pin_index = upf_self()->dataplane.worker_count +
        (dispatcher->type == UPF_DISPATCH_N6);

    tls_worker_id = 0;
    pin_current_thread_when_ready(pin_index, dispatcher->name);
    ogs_info("UPF %s started", dispatcher->name);
    while (!is_dispatcher_stopping()) {
#if HAVE_LIBMEMIF
        struct epoll_event events[UPF_EPOLL_MAX_EVENTS];
        bool pending = dispatcher->type == UPF_DISPATCH_N3 ?
            upf_n3_memif_has_pending() : upf_n6_memif_has_pending();
        int timeout_ms = pending ? 0 : 1000;
        int count;
        int i;

        count = epoll_wait(dispatcher->epoll_fd,
                events, OGS_ARRAY_SIZE(events), timeout_ms);
        dispatcher->epoll_waits++;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            dispatcher->epoll_errors++;
            ogs_error("UPF memif epoll_wait() failed: %s", strerror(errno));
            ogs_msleep(1);
            continue;
        }
        dispatcher->epoll_events += count;
        for (i = 0; i < count; i++) {
            memif_fd_event_type_t memif_events = 0;
            int rv;

            if (events[i].data.ptr == &dispatcher->wake_token) {
                uint64_t value;

                while (read(dispatcher->wake_fd,
                            &value, sizeof(value)) == sizeof(value))
                    dispatcher->epoll_wakeups += value;
                continue;
            }
            if (events[i].events & EPOLLIN)
                memif_events |= MEMIF_FD_EVENT_READ;
            if (events[i].events & EPOLLOUT)
                memif_events |= MEMIF_FD_EVENT_WRITE;
            if (events[i].events & (EPOLLERR | EPOLLHUP))
                memif_events |= MEMIF_FD_EVENT_ERROR;
            rv = memif_control_fd_handler(
                    events[i].data.ptr, memif_events);
            if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_AGAIN) {
                dispatcher->epoll_errors++;
                ogs_warn("memif_control_fd_handler() failed: %s",
                        memif_strerror(rv));
            }
        }

        if (!is_dispatcher_stopping()) {
            int rv;

            if (!upf_self()->dataplane.session_workers) {
                pthread_mutex_lock(&legacy_io_lock);
                upf_dataplane_read_lock();
            }
            if (dispatcher->type == UPF_DISPATCH_N6)
                rv = upf_n6_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
            else
                rv = upf_n3_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
            if (rv == OGS_ERROR)
                dispatcher->epoll_errors++;
            if (!upf_self()->dataplane.session_workers) {
                upf_dataplane_read_unlock();
                pthread_mutex_unlock(&legacy_io_lock);
            }
        }
#else
        ogs_msleep(1000);
#endif

        if (ogs_get_monotonic_time() - last_stats >=
                ogs_time_from_sec(upf_self()->dataplane.stats_interval)) {
            uint8_t i;

#if HAVE_LIBMEMIF
            ogs_info("UPF %s epoll stats "
                    "[wait:%llu events:%llu wake:%llu errors:%llu]",
                    dispatcher->name,
                    (unsigned long long)dispatcher->epoll_waits,
                    (unsigned long long)dispatcher->epoll_events,
                    (unsigned long long)dispatcher->epoll_wakeups,
                    (unsigned long long)dispatcher->epoll_errors);
#endif
            if (dispatcher->type == UPF_DISPATCH_N3)
                upf_n3_memif_log_stats();
            else
                upf_n6_memif_log_stats();
            if (dispatcher->type != UPF_DISPATCH_N3) {
                last_stats = ogs_get_monotonic_time();
                continue;
            }
            for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
                uint64_t read_seq = __atomic_load_n(
                        &workers[i].read_seq, __ATOMIC_ACQUIRE);

                ogs_info("UPF worker %u stats "
                        "[packets:%llu drops:%llu queue-depth:%llu "
                        "queue-high:%llu queue-full:%llu push-fail:%llu]",
                        i,
                        (unsigned long long)__atomic_load_n(
                            &workers[i].packets, __ATOMIC_RELAXED),
                        (unsigned long long)workers[i].drops,
                        (unsigned long long)(workers[i].write_seq - read_seq),
                        (unsigned long long)workers[i].queue_high_water,
                        (unsigned long long)workers[i].queue_full_batches,
                        (unsigned long long)workers[i].queue_push_failures);
                ogs_info("UPF worker %u wait stats "
                        "[mode:%d blocking:%llu poll-hit:%llu "
                        "poll-loop:%llu]",
                        i, upf_self()->dataplane.worker_busy_poll_us,
                        (unsigned long long)workers[i].blocking_waits,
                        (unsigned long long)workers[i].poll_hits,
                        (unsigned long long)workers[i].poll_loops);
            }
            last_stats = ogs_get_monotonic_time();
        }
    }
    ogs_info("UPF %s stopped", dispatcher->name);
}

static uint8_t owner_from_n3_locked(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t teid;
    ogs_pfcp_object_t *object;
    ogs_pfcp_sess_t *pfcp_sess = NULL;
    upf_sess_t *sess = NULL;

    if (len < 8 || (p[0] >> 5) != OGS_GTP2_VERSION_1)
        return 0;
    memcpy(&teid, p + 4, sizeof(teid));
    teid = be32toh(teid);
    if (!teid)
        return 0;

    object = ogs_pfcp_object_find_by_teid(teid);
    if (object) {
        if (object->type == OGS_PFCP_OBJ_SESS_TYPE)
            pfcp_sess = (ogs_pfcp_sess_t *)object;
        else if (object->type == OGS_PFCP_OBJ_PDR_TYPE)
            pfcp_sess = ((ogs_pfcp_pdr_t *)object)->sess;
        if (pfcp_sess)
            sess = UPF_SESS(pfcp_sess);
    }
    teid = sess ? sess->owner_worker : 0;
    return (uint8_t)teid;
}

static uint8_t owner_from_n6_locked(const void *data, size_t len)
{
    const uint8_t *p = data;
    upf_sess_t *sess = NULL;
    uint8_t owner = 0;

    if (len >= sizeof(struct ip) && (p[0] >> 4) == 4) {
        uint32_t addr;
        memcpy(&addr, p + 16, sizeof(addr));
        sess = upf_sess_find_by_ipv4(addr);
    } else if (len >= sizeof(struct ip6_hdr) && (p[0] >> 4) == 6) {
        uint32_t addr6[4];
        memcpy(addr6, p + 24, sizeof(addr6));
        sess = upf_sess_find_by_ipv6(addr6);
    }
    if (sess)
        owner = sess->owner_worker;
    return owner;
}

static int submit_batch(upf_work_type_t type,
        const upf_dataplane_packet_t packets[], uint16_t count,
        uint16_t *submitted)
{
    uint16_t indices[UPF_MAX_WORKERS][UPF_DATAPLANE_MAX_BURST];
    uint16_t owner_count[UPF_MAX_WORKERS] = { 0 };
    uint8_t worker_count = upf_self()->dataplane.worker_count;
    uint16_t accepted = 0;
    uint16_t i;
    uint8_t owner;
    int result = OGS_OK;

    if (submitted)
        *submitted = 0;
    if (!packets || !count || count > UPF_DATAPLANE_MAX_BURST)
        return OGS_ERROR;

    /*
     * Session rules are stable for this complete RX burst. Grouping indices
     * preserves packet order within each session owner.
     */
    upf_dataplane_read_lock();
    for (i = 0; i < count; i++) {
        if (!packets[i].data || !packets[i].len) {
            if (packets[i].complete)
                packets[i].complete(packets[i].qid,
                        packets[i].sequence, packets[i].generation);
            result = OGS_ERROR;
            continue;
        }
        owner = type == UPF_WORK_N3 ?
            owner_from_n3_locked(packets[i].data, packets[i].len) :
            owner_from_n6_locked(packets[i].data, packets[i].len);
        if (owner >= worker_count ||
            ((!packets[i].complete || packets[i].force_copy) &&
             sizeof(upf_work_t) + packets[i].len >
                workers[owner].task_size)) {
            if (packets[i].complete)
                packets[i].complete(packets[i].qid,
                        packets[i].sequence, packets[i].generation);
            result = OGS_ERROR;
            continue;
        }
        indices[owner][owner_count[owner]++] = i;
        accepted++;
    }
    upf_dataplane_read_unlock();

    for (owner = 0; owner < worker_count; owner++) {
        upf_worker_t *worker = &workers[owner];
        upf_work_batch_t *batch;
        uint64_t read_seq;
        uint64_t start;
        uint16_t j;

        if (!owner_count[owner])
            continue;
        pthread_mutex_lock(&worker->submit_lock);
        read_seq = __atomic_load_n(&worker->read_seq, __ATOMIC_ACQUIRE);
        if (worker->write_seq - read_seq >
                upf_self()->dataplane.worker_queue_size -
                owner_count[owner] ||
            ogs_queue_trypop(
                worker->free_queue, (void **)&batch) != OGS_OK) {
            for (j = 0; j < owner_count[owner]; j++) {
                const upf_dataplane_packet_t *packet =
                    &packets[indices[owner][j]];

                if (packet->complete)
                    packet->complete(packet->qid,
                            packet->sequence, packet->generation);
            }
            worker->drops += owner_count[owner];
            worker->queue_full_batches++;
            result = OGS_ERROR;
            pthread_mutex_unlock(&worker->submit_lock);
            continue;
        }

        start = worker->write_seq;
        for (j = 0; j < owner_count[owner]; j++) {
            const upf_dataplane_packet_t *packet =
                &packets[indices[owner][j]];
            uint64_t seq = start + j;
            upf_work_t *work = (upf_work_t *)(
                    (uint8_t *)worker->task_pool +
                    (seq % upf_self()->dataplane.worker_queue_size) *
                    worker->task_size);

            work->type = type;
            work->len = packet->len;
            work->lease_data = NULL;
            work->complete = NULL;
            if (type == UPF_WORK_N3)
                memcpy(&work->from, &packet->from, sizeof(work->from));
            if (packet->complete && !packet->force_copy) {
                work->lease_data = packet->data;
                work->complete = packet->complete;
                work->qid = packet->qid;
                work->sequence = packet->sequence;
                work->generation = packet->generation;
            } else {
                memcpy(work->data, packet->data, packet->len);
                if (packet->complete)
                    packet->complete(packet->qid,
                            packet->sequence, packet->generation);
            }
        }
        batch->start = start;
        batch->count = owner_count[owner];
        worker->write_seq = start + owner_count[owner];
        if (worker->write_seq - read_seq > worker->queue_high_water)
            worker->queue_high_water = worker->write_seq - read_seq;
        if (ogs_queue_trypush(worker->queue, batch) != OGS_OK) {
            for (j = 0; j < owner_count[owner]; j++) {
                uint64_t seq = start + j;
                upf_work_t *work = (upf_work_t *)(
                        (uint8_t *)worker->task_pool +
                        (seq % upf_self()->dataplane.worker_queue_size) *
                        worker->task_size);

                if (work->complete)
                    work->complete(work->qid,
                            work->sequence, work->generation);
            }
            worker->write_seq = start;
            worker->drops += owner_count[owner];
            worker->queue_push_failures++;
            ogs_assert(ogs_queue_trypush(
                        worker->free_queue, batch) == OGS_OK);
            result = OGS_ERROR;
            pthread_mutex_unlock(&worker->submit_lock);
            continue;
        }
        if (submitted)
            *submitted += owner_count[owner];
        pthread_mutex_unlock(&worker->submit_lock);
    }

    return accepted == count ? result : OGS_ERROR;
}

int upf_dataplane_submit_n3(
        const void *data, size_t len, const ogs_sockaddr_t *from)
{
    upf_dataplane_packet_t packet = {
        .data = data,
        .len = len,
    };

    if (from)
        memcpy(&packet.from, from, sizeof(packet.from));
    return submit_batch(UPF_WORK_N3, &packet, 1, NULL);
}

int upf_dataplane_submit_n6(const void *data, size_t len)
{
    upf_dataplane_packet_t packet = {
        .data = data,
        .len = len,
    };

    return submit_batch(UPF_WORK_N6, &packet, 1, NULL);
}

int upf_dataplane_submit_n3_batch(
        const upf_dataplane_packet_t packets[], uint16_t count,
        uint16_t *submitted)
{
    return submit_batch(UPF_WORK_N3, packets, count, submitted);
}

int upf_dataplane_submit_n6_batch(
        const upf_dataplane_packet_t packets[], uint16_t count,
        uint16_t *submitted)
{
    return submit_batch(UPF_WORK_N6, packets, count, submitted);
}

#if HAVE_LIBMEMIF
static int ensure_dispatcher_epoll(upf_dispatcher_t *dispatcher)
{
    struct epoll_event event;

    if (dispatcher->epoll_fd >= 0)
        return OGS_OK;

    dispatcher->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (dispatcher->epoll_fd < 0) {
        ogs_error("epoll_create1() failed: %s", strerror(errno));
        return OGS_ERROR;
    }
    dispatcher->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (dispatcher->wake_fd < 0) {
        ogs_error("eventfd() failed: %s", strerror(errno));
        close(dispatcher->epoll_fd);
        dispatcher->epoll_fd = -1;
        return OGS_ERROR;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = &dispatcher->wake_token;
    if (epoll_ctl(dispatcher->epoll_fd, EPOLL_CTL_ADD,
                dispatcher->wake_fd, &event) < 0) {
        ogs_error("epoll_ctl(wake-fd) failed: %s", strerror(errno));
        close(dispatcher->wake_fd);
        close(dispatcher->epoll_fd);
        dispatcher->wake_fd = -1;
        dispatcher->epoll_fd = -1;
        return OGS_ERROR;
    }
    return OGS_OK;
}

static int memif_fd_update(
        upf_dispatcher_t *dispatcher, memif_fd_event_t fde)
{
    struct epoll_event event;
    int operation = EPOLL_CTL_ADD;

    if (ensure_dispatcher_epoll(dispatcher) != OGS_OK)
        return MEMIF_ERR_CB_FDUPDATE;
    if (fde.fd < 0)
        return MEMIF_ERR_BAD_FD;

    if (fde.type & MEMIF_FD_EVENT_DEL) {
        if (epoll_ctl(dispatcher->epoll_fd,
                    EPOLL_CTL_DEL, fde.fd, NULL) < 0 && errno != ENOENT) {
            ogs_error("epoll_ctl(DEL fd:%d) failed: %s",
                    fde.fd, strerror(errno));
            return MEMIF_ERR_CB_FDUPDATE;
        }
        return MEMIF_ERR_SUCCESS;
    }

    memset(&event, 0, sizeof(event));
    if (fde.type & MEMIF_FD_EVENT_READ)
        event.events |= EPOLLIN;
    if (fde.type & MEMIF_FD_EVENT_WRITE)
        event.events |= EPOLLOUT;
    event.data.ptr = fde.private_ctx;
    if (fde.type & MEMIF_FD_EVENT_MOD)
        operation = EPOLL_CTL_MOD;
    if (epoll_ctl(dispatcher->epoll_fd,
                operation, fde.fd, &event) < 0) {
        ogs_error("epoll_ctl(%s fd:%d) failed: %s",
                operation == EPOLL_CTL_MOD ? "MOD" : "ADD",
                fde.fd, strerror(errno));
        return MEMIF_ERR_CB_FDUPDATE;
    }
    return MEMIF_ERR_SUCCESS;
}

int upf_dataplane_n3_memif_fd_update(
        memif_fd_event_t fde, void *private_ctx)
{
    (void)private_ctx;
    return memif_fd_update(&dispatchers[UPF_DISPATCH_N3], fde);
}

int upf_dataplane_n6_memif_fd_update(
        memif_fd_event_t fde, void *private_ctx)
{
    (void)private_ctx;
    return memif_fd_update(&dispatchers[UPF_DISPATCH_N6], fde);
}
#endif

void upf_dataplane_wake(void)
{
#if HAVE_LIBMEMIF
    uint8_t i;
    uint64_t value = 1;

    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        if (dispatchers[i].wake_fd >= 0 &&
            write(dispatchers[i].wake_fd, &value, sizeof(value)) < 0 &&
            errno != EAGAIN)
            ogs_warn("UPF %s wake failed: %s",
                    dispatchers[i].name, strerror(errno));
    }
#endif
}

void upf_dataplane_init(void)
{
    uint8_t i;

    pthread_rwlock_init(&rule_lock, NULL);
    pthread_mutex_init(&report_lock, NULL);
    pthread_mutex_init(&legacy_io_lock, NULL);
    memset(owned_sessions, 0, sizeof(owned_sessions));
    memset(dispatchers, 0, sizeof(dispatchers));
    dispatchers[UPF_DISPATCH_N3].type = UPF_DISPATCH_N3;
    dispatchers[UPF_DISPATCH_N3].name = "N3 memif dispatcher";
    dispatchers[UPF_DISPATCH_N6].type = UPF_DISPATCH_N6;
    dispatchers[UPF_DISPATCH_N6].name = "N6 memif dispatcher";
#if HAVE_LIBMEMIF
    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        dispatchers[i].epoll_fd = -1;
        dispatchers[i].wake_fd = -1;
    }
#else
    (void)i;
#endif
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dispatcher_stopping, 0, __ATOMIC_RELEASE);
}

void upf_dataplane_final(void)
{
    uint8_t i;

    for (i = 0; i < UPF_DISPATCH_MAX; i++)
        ogs_assert(!dispatchers[i].thread);
#if HAVE_LIBMEMIF
    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        if (dispatchers[i].wake_fd >= 0) {
            close(dispatchers[i].wake_fd);
            dispatchers[i].wake_fd = -1;
        }
        if (dispatchers[i].epoll_fd >= 0) {
            close(dispatchers[i].epoll_fd);
            dispatchers[i].epoll_fd = -1;
        }
    }
#endif
    pthread_mutex_destroy(&report_lock);
    pthread_mutex_destroy(&legacy_io_lock);
    pthread_rwlock_destroy(&rule_lock);
}

int upf_dataplane_start(void)
{
    uint8_t i;
    uint32_t j;
    size_t max_packet_size = upf_self()->n3.buffer_size;

    for (i = 0; i < UPF_DISPATCH_MAX; i++)
        ogs_assert(!dispatchers[i].thread);
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&dispatcher_stopping, 0, __ATOMIC_RELEASE);

#if HAVE_LIBMEMIF
    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        if (ensure_dispatcher_epoll(&dispatchers[i]) != OGS_OK)
            return OGS_ERROR;
        dispatchers[i].epoll_waits = 0;
        dispatchers[i].epoll_events = 0;
        dispatchers[i].epoll_errors = 0;
        dispatchers[i].epoll_wakeups = 0;
    }
#endif
    if (upf_self()->n6.buffer_size > max_packet_size)
        max_packet_size = upf_self()->n6.buffer_size;
    for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
        workers[i].id = i;
        pthread_mutex_init(&workers[i].submit_lock, NULL);
        workers[i].submit_lock_initialized = true;
        workers[i].task_size = sizeof(upf_work_t) + max_packet_size;
        workers[i].queue =
            ogs_queue_create(upf_self()->dataplane.worker_queue_size);
        workers[i].free_queue =
            ogs_queue_create(upf_self()->dataplane.worker_queue_size);
        workers[i].task_pool = ogs_calloc(
                upf_self()->dataplane.worker_queue_size,
                workers[i].task_size);
        workers[i].batch_pool = ogs_calloc(
                upf_self()->dataplane.worker_queue_size,
                sizeof(upf_work_batch_t));
        if (!workers[i].queue || !workers[i].free_queue ||
            !workers[i].task_pool || !workers[i].batch_pool)
            goto fail;
        workers[i].write_seq = 0;
        __atomic_store_n(&workers[i].read_seq, 0, __ATOMIC_RELEASE);
        workers[i].packets = 0;
        workers[i].drops = 0;
        workers[i].queue_full_batches = 0;
        workers[i].queue_push_failures = 0;
        workers[i].queue_high_water = 0;
        workers[i].blocking_waits = 0;
        workers[i].poll_hits = 0;
        workers[i].poll_loops = 0;
        for (j = 0; j < upf_self()->dataplane.worker_queue_size; j++)
            ogs_assert(ogs_queue_trypush(workers[i].free_queue,
                        (upf_work_batch_t *)workers[i].batch_pool + j)
                    == OGS_OK);
        workers[i].thread = ogs_thread_create(session_worker_main, &workers[i]);
        if (!workers[i].thread)
            goto fail;
    }
    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        dispatchers[i].thread =
            ogs_thread_create(dispatcher_main, &dispatchers[i]);
        if (!dispatchers[i].thread)
            goto fail;
    }
    return OGS_OK;

fail:
    upf_dataplane_stop();
    return OGS_ERROR;
}

void upf_dataplane_stop(void)
{
    uint8_t i;
    int retry;

    __atomic_store_n(&dispatcher_stopping, 1, __ATOMIC_RELEASE);
    upf_dataplane_wake();
    for (i = 0; i < UPF_DISPATCH_MAX; i++) {
        if (dispatchers[i].thread) {
            ogs_thread_destroy(dispatchers[i].thread);
            dispatchers[i].thread = NULL;
        }
    }

    /*
     * RX is stopped now. Give workers a bounded interval to finish all
     * descriptor leases before terminating their blocking queues.
     */
    for (retry = 0; retry < 1000; retry++) {
        bool drained = true;

        for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
            if (__atomic_load_n(
                        &workers[i].read_seq, __ATOMIC_ACQUIRE) !=
                    workers[i].write_seq) {
                drained = false;
                break;
            }
        }
        if (drained)
            break;
        ogs_msleep(1);
    }
    upf_n3_memif_drain_completions();
    upf_n6_memif_drain_completions();

    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    for (i = 0; i < UPF_MAX_WORKERS; i++) {
        if (workers[i].queue)
            ogs_queue_term(workers[i].queue);
        if (workers[i].thread) {
            ogs_thread_destroy(workers[i].thread);
            workers[i].thread = NULL;
        }
        if (workers[i].queue) {
            ogs_queue_destroy(workers[i].queue);
            workers[i].queue = NULL;
        }
        if (workers[i].free_queue) {
            ogs_queue_term(workers[i].free_queue);
            ogs_queue_destroy(workers[i].free_queue);
            workers[i].free_queue = NULL;
        }
        if (workers[i].task_pool) {
            ogs_free(workers[i].task_pool);
            workers[i].task_pool = NULL;
        }
        if (workers[i].batch_pool) {
            ogs_free(workers[i].batch_pool);
            workers[i].batch_pool = NULL;
        }
        if (workers[i].submit_lock_initialized) {
            pthread_mutex_destroy(&workers[i].submit_lock);
            workers[i].submit_lock_initialized = false;
        }
    }
}

void upf_dataplane_lock(void)
{
    pthread_rwlock_wrlock(&rule_lock);
}

void upf_dataplane_unlock(void)
{
    pthread_rwlock_unlock(&rule_lock);
}

void upf_dataplane_read_lock(void)
{
    pthread_rwlock_rdlock(&rule_lock);
}

void upf_dataplane_read_unlock(void)
{
    pthread_rwlock_unlock(&rule_lock);
}

uint8_t upf_dataplane_worker_id(void)
{
    return tls_worker_id;
}

uint8_t upf_dataplane_assign_session(uint64_t seid)
{
    uint8_t count = upf_self()->dataplane.worker_count;
    uint64_t mixed = seid ^ (seid >> 33) ^ (seid << 11);
    uint8_t start = mixed % count;
    uint8_t owner = start;
    unsigned int owner_count;
    uint8_t offset;

    owner_count = __atomic_load_n(
            &owned_sessions[owner], __ATOMIC_RELAXED);
    for (offset = 1; offset < count; offset++) {
        uint8_t candidate = (start + offset) % count;
        unsigned int candidate_count = __atomic_load_n(
                &owned_sessions[candidate], __ATOMIC_RELAXED);

        if (candidate_count < owner_count) {
            owner = candidate;
            owner_count = candidate_count;
        }
    }
    __atomic_fetch_add(&owned_sessions[owner], 1, __ATOMIC_RELAXED);
    return owner;
}

void upf_dataplane_release_session(uint8_t owner)
{
    if (owner < UPF_MAX_WORKERS)
        __atomic_fetch_sub(&owned_sessions[owner], 1, __ATOMIC_RELAXED);
}

void upf_dataplane_report_lock(void)
{
    pthread_mutex_lock(&report_lock);
}

void upf_dataplane_report_unlock(void)
{
    pthread_mutex_unlock(&report_lock);
}
