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
#include <poll.h>
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
    uint8_t data[];
} upf_work_t;

typedef struct {
    uint64_t start;
    uint16_t count;
} upf_work_batch_t;

typedef struct {
    uint8_t id;
    ogs_thread_t *thread;
    void *task_pool;
    upf_work_batch_t *batch_ring;
    size_t task_size;
    /* Keep producer- and consumer-owned cursors on separate cache lines. */
    uint64_t write_seq __attribute__((aligned(64)));
    uint64_t batch_write_seq;
    uint8_t producer_cursor_pad[48];
    uint64_t read_seq;
    uint64_t batch_read_seq;
    uint8_t consumer_cursor_pad[48];
    uint64_t packets;
    uint64_t drops;
    uint64_t ring_full_batches;
    uint64_t queue_high_water;
    uint64_t batch_high_water;
#if HAVE_LIBMEMIF
    int wake_fd;
    unsigned int sleeping;
    uint64_t wake_signals;
    uint64_t wake_events;
    uint64_t sleeps;
    uint64_t busy_loops;
#endif
} upf_worker_t;

static ogs_thread_t *dispatcher;
static upf_worker_t workers[UPF_MAX_WORKERS];
static pthread_rwlock_t rule_lock;
static pthread_mutex_t report_lock;
static unsigned int owned_sessions[UPF_MAX_WORKERS];
static int stopping;
static __thread uint8_t tls_worker_id;

#if HAVE_LIBMEMIF
static int dispatcher_epoll_fd = -1;
static int dispatcher_wake_fd = -1;
static uint8_t dispatcher_wake_token;
static uint64_t epoll_waits;
static uint64_t epoll_events;
static uint64_t epoll_errors;
static uint64_t epoll_wakeups;
#endif

static bool is_stopping(void)
{
    return __atomic_load_n(&stopping, __ATOMIC_ACQUIRE);
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

static bool worker_batch_ring_empty(upf_worker_t *worker)
{
    uint64_t head = __atomic_load_n(
            &worker->batch_read_seq, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(
            &worker->batch_write_seq, __ATOMIC_ACQUIRE);

    return head == tail;
}

static bool worker_batch_push(
        upf_worker_t *worker, uint64_t start, uint16_t count)
{
    uint64_t head = __atomic_load_n(
            &worker->batch_read_seq, __ATOMIC_ACQUIRE);
    uint64_t tail = __atomic_load_n(
            &worker->batch_write_seq, __ATOMIC_RELAXED);
    uint64_t depth;
    upf_work_batch_t *batch;

    if (tail - head >= upf_self()->dataplane.worker_queue_size)
        return false;

    batch = &worker->batch_ring[
        tail % upf_self()->dataplane.worker_queue_size];
    batch->start = start;
    batch->count = count;
    __atomic_store_n(
            &worker->batch_write_seq, tail + 1, __ATOMIC_RELEASE);

    depth = tail + 1 - head;
    if (depth > worker->batch_high_water)
        worker->batch_high_water = depth;

#if HAVE_LIBMEMIF
    /*
     * The consumer sets sleeping before rechecking the ring. Publishing the
     * batch first and fencing before loading sleeping prevents a lost wake-up.
     */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (__atomic_load_n(&worker->sleeping, __ATOMIC_ACQUIRE)) {
        uint64_t value = 1;

        if (write(worker->wake_fd, &value, sizeof(value)) == sizeof(value))
            worker->wake_signals++;
        else if (errno != EAGAIN)
            ogs_warn("UPF worker %u wake failed: %s",
                    worker->id, strerror(errno));
    }
#endif
    return true;
}

static bool worker_batch_pop(
        upf_worker_t *worker, upf_work_batch_t *batch)
{
    uint64_t head = __atomic_load_n(
            &worker->batch_read_seq, __ATOMIC_RELAXED);
    uint64_t tail = __atomic_load_n(
            &worker->batch_write_seq, __ATOMIC_ACQUIRE);

    if (head == tail)
        return false;

    *batch = worker->batch_ring[
        head % upf_self()->dataplane.worker_queue_size];
    __atomic_store_n(
            &worker->batch_read_seq, head + 1, __ATOMIC_RELEASE);
    return true;
}

static void worker_cpu_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ volatile("yield");
#else
    __asm__ volatile("" ::: "memory");
#endif
}

static void worker_wait_for_batch(upf_worker_t *worker)
{
#if HAVE_LIBMEMIF
    struct pollfd pfd;
    int rv;

    __atomic_store_n(&worker->sleeping, 1, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!worker_batch_ring_empty(worker) || is_stopping()) {
        __atomic_store_n(&worker->sleeping, 0, __ATOMIC_RELEASE);
        return;
    }

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = worker->wake_fd;
    pfd.events = POLLIN;
    __atomic_fetch_add(&worker->sleeps, 1, __ATOMIC_RELAXED);
    do {
        rv = poll(&pfd, 1, -1);
    } while (rv < 0 && errno == EINTR && !is_stopping());

    if (rv > 0 && (pfd.revents & POLLIN)) {
        uint64_t value;

        if (read(worker->wake_fd, &value, sizeof(value)) == sizeof(value))
            __atomic_fetch_add(
                    &worker->wake_events, value, __ATOMIC_RELAXED);
    } else if (rv < 0 && !is_stopping()) {
        ogs_warn("UPF worker %u poll failed: %s",
                worker->id, strerror(errno));
        ogs_msleep(1);
    }
    __atomic_store_n(&worker->sleeping, 0, __ATOMIC_RELEASE);
#else
    ogs_msleep(1);
#endif
}

static void session_worker_main(void *data)
{
    upf_worker_t *worker = data;
    upf_work_batch_t pending;
    bool has_pending = false;
    ogs_time_t busy_until = 0;
    uint64_t busy_loops = 0;

    tls_worker_id = worker->id;
    pin_current_thread_when_ready(worker->id, "session-worker");
    ogs_info("UPF session worker %u started", worker->id);
    while (!is_stopping()) {
        upf_work_batch_t batches[UPF_DATAPLANE_MAX_BURST];
        unsigned int batch_count = 0;
        unsigned int packet_count = 0;
        unsigned int n6_count = 0;
        unsigned int i;

        if (has_pending) {
            batches[batch_count++] = pending;
            packet_count = pending.count;
            has_pending = false;
        } else if (worker_batch_pop(worker, &batches[0])) {
            packet_count = batches[0].count;
            batch_count = 1;
        } else {
            if (busy_until &&
                ogs_get_monotonic_time() < busy_until) {
                worker_cpu_relax();
                busy_loops++;
                continue;
            }
            if (busy_loops) {
                __atomic_fetch_add(&worker->busy_loops,
                        busy_loops, __ATOMIC_RELAXED);
                busy_loops = 0;
            }
            busy_until = 0;
            worker_wait_for_batch(worker);
            continue;
        }

        while (batch_count < UPF_DATAPLANE_MAX_BURST &&
                packet_count < UPF_DATAPLANE_MAX_BURST) {
            upf_work_batch_t next;

            if (!worker_batch_pop(worker, &next))
                break;
            if (packet_count + next.count > UPF_DATAPLANE_MAX_BURST) {
                pending = next;
                has_pending = true;
                break;
            }
            batches[batch_count++] = next;
            packet_count += next.count;
        }

        for (i = 0; i < batch_count; i++) {
            upf_work_batch_t *batch = &batches[i];
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
            upf_work_batch_t *batch = &batches[i];
            unsigned int j;

            for (j = 0; j < batch->count; j++) {
                uint64_t seq = batch->start + j;
                upf_work_t *work = (upf_work_t *)(
                        (uint8_t *)worker->task_pool +
                        (seq % upf_self()->dataplane.worker_queue_size) *
                        worker->task_size);
                if (work->type == UPF_WORK_N3)
                    upf_gtp_handle_n3_data(
                            work->data, work->len, &work->from);
                else
                    upf_gtp_handle_n6_data(work->data, work->len);
            }
        }
        upf_n6_memif_tx_batch_flush();
        upf_n3_memif_tx_batch_flush();
        upf_dataplane_read_unlock();
        __atomic_fetch_add(&worker->packets,
                packet_count, __ATOMIC_RELAXED);
        for (i = 0; i < batch_count; i++) {
            upf_work_batch_t *batch = &batches[i];

            __atomic_store_n(&worker->read_seq,
                    batch->start + batch->count, __ATOMIC_RELEASE);
        }
        busy_until = ogs_get_monotonic_time() +
            upf_self()->dataplane.worker_busy_poll_us;
    }
    if (busy_loops)
        __atomic_fetch_add(&worker->busy_loops,
                busy_loops, __ATOMIC_RELAXED);
    ogs_info("UPF session worker %u stopped [packets:%llu drops:%llu]",
            worker->id, (unsigned long long)worker->packets,
            (unsigned long long)worker->drops);
}

static void dispatcher_main(void *data)
{
    ogs_time_t last_stats = ogs_get_monotonic_time();
    bool n6_first = false;

    tls_worker_id = 0;
    pin_current_thread_when_ready(
            upf_self()->dataplane.worker_count, "memif-dispatcher");
    ogs_info("UPF memif dispatcher started");
    while (!is_stopping()) {
#if HAVE_LIBMEMIF
        struct epoll_event events[UPF_EPOLL_MAX_EVENTS];
        bool pending =
            upf_n3_memif_has_pending() || upf_n6_memif_has_pending();
        int timeout_ms = pending ? 0 : 1000;
        int count;
        int i;

        count = epoll_wait(dispatcher_epoll_fd,
                events, OGS_ARRAY_SIZE(events), timeout_ms);
        epoll_waits++;
        if (count < 0) {
            if (errno == EINTR)
                continue;
            epoll_errors++;
            ogs_error("UPF memif epoll_wait() failed: %s", strerror(errno));
            ogs_msleep(1);
            continue;
        }
        epoll_events += count;
        for (i = 0; i < count; i++) {
            memif_fd_event_type_t memif_events = 0;
            int rv;

            if (events[i].data.ptr == &dispatcher_wake_token) {
                uint64_t value;

                while (read(dispatcher_wake_fd,
                            &value, sizeof(value)) == sizeof(value))
                    epoll_wakeups += value;
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
                epoll_errors++;
                ogs_warn("memif_control_fd_handler() failed: %s",
                        memif_strerror(rv));
            }
        }

        if (!is_stopping()) {
            int rv;

            /*
             * One bounded qid quantum per direction. Flip the first direction
             * after every round so a continuously busy ring cannot starve the
             * other socket.
             */
            if (!upf_self()->dataplane.session_workers)
                upf_dataplane_read_lock();
            if (n6_first) {
                rv = upf_n6_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
                if (rv == OGS_ERROR)
                    epoll_errors++;
                rv = upf_n3_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
                if (rv == OGS_ERROR)
                    epoll_errors++;
            } else {
                rv = upf_n3_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
                if (rv == OGS_ERROR)
                    epoll_errors++;
                rv = upf_n6_memif_service(
                        upf_self()->dataplane.io_packet_budget,
                        upf_self()->dataplane.io_time_budget_us);
                if (rv == OGS_ERROR)
                    epoll_errors++;
            }
            if (!upf_self()->dataplane.session_workers)
                upf_dataplane_read_unlock();
            n6_first = !n6_first;
        }
#else
        ogs_msleep(1000);
#endif

        if (ogs_get_monotonic_time() - last_stats >=
                ogs_time_from_sec(upf_self()->dataplane.stats_interval)) {
            uint8_t i;

#if HAVE_LIBMEMIF
            ogs_info("UPF memif epoll stats "
                    "[wait:%llu events:%llu wake:%llu errors:%llu]",
                    (unsigned long long)epoll_waits,
                    (unsigned long long)epoll_events,
                    (unsigned long long)epoll_wakeups,
                    (unsigned long long)epoll_errors);
#endif
            upf_n3_memif_log_stats();
            upf_n6_memif_log_stats();
            for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
                uint64_t read_seq = __atomic_load_n(
                        &workers[i].read_seq, __ATOMIC_ACQUIRE);
                uint64_t batch_read_seq = __atomic_load_n(
                        &workers[i].batch_read_seq, __ATOMIC_ACQUIRE);
                uint64_t batch_write_seq = __atomic_load_n(
                        &workers[i].batch_write_seq, __ATOMIC_ACQUIRE);

                ogs_info("UPF worker %u stats "
                        "[packets:%llu drops:%llu task-depth:%llu "
                        "task-high:%llu batch-depth:%llu batch-high:%llu "
                        "ring-full:%llu busy-loops:%llu sleeps:%llu "
                        "wake-signals:%llu wake-events:%llu]",
                        i,
                        (unsigned long long)__atomic_load_n(
                            &workers[i].packets, __ATOMIC_RELAXED),
                        (unsigned long long)workers[i].drops,
                        (unsigned long long)(workers[i].write_seq - read_seq),
                        (unsigned long long)workers[i].queue_high_water,
                        (unsigned long long)(
                            batch_write_seq - batch_read_seq),
                        (unsigned long long)workers[i].batch_high_water,
                        (unsigned long long)workers[i].ring_full_batches,
#if HAVE_LIBMEMIF
                        (unsigned long long)__atomic_load_n(
                            &workers[i].busy_loops, __ATOMIC_RELAXED),
                        (unsigned long long)__atomic_load_n(
                            &workers[i].sleeps, __ATOMIC_RELAXED),
                        (unsigned long long)workers[i].wake_signals,
                        (unsigned long long)__atomic_load_n(
                            &workers[i].wake_events, __ATOMIC_RELAXED)
#else
                        0ULL, 0ULL, 0ULL, 0ULL
#endif
                        );
            }
            last_stats = ogs_get_monotonic_time();
        }
    }
    ogs_info("UPF memif dispatcher stopped");
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
            result = OGS_ERROR;
            continue;
        }
        owner = type == UPF_WORK_N3 ?
            owner_from_n3_locked(packets[i].data, packets[i].len) :
            owner_from_n6_locked(packets[i].data, packets[i].len);
        if (owner >= worker_count ||
            sizeof(upf_work_t) + packets[i].len >
                workers[owner].task_size) {
            result = OGS_ERROR;
            continue;
        }
        indices[owner][owner_count[owner]++] = i;
        accepted++;
    }
    upf_dataplane_read_unlock();

    for (owner = 0; owner < worker_count; owner++) {
        upf_worker_t *worker = &workers[owner];
        uint64_t read_seq;
        uint64_t batch_read_seq;
        uint64_t batch_write_seq;
        uint64_t start;
        uint16_t j;

        if (!owner_count[owner])
            continue;
        read_seq = __atomic_load_n(&worker->read_seq, __ATOMIC_ACQUIRE);
        batch_read_seq = __atomic_load_n(
                &worker->batch_read_seq, __ATOMIC_ACQUIRE);
        batch_write_seq = __atomic_load_n(
                &worker->batch_write_seq, __ATOMIC_RELAXED);
        if (worker->write_seq - read_seq >
                upf_self()->dataplane.worker_queue_size -
                owner_count[owner] ||
            batch_write_seq - batch_read_seq >=
                upf_self()->dataplane.worker_queue_size) {
            worker->drops += owner_count[owner];
            worker->ring_full_batches++;
            result = OGS_ERROR;
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
            if (type == UPF_WORK_N3)
                memcpy(&work->from, &packet->from, sizeof(work->from));
            memcpy(work->data, packet->data, packet->len);
        }
        worker->write_seq = start + owner_count[owner];
        if (worker->write_seq - read_seq > worker->queue_high_water)
            worker->queue_high_water = worker->write_seq - read_seq;
        ogs_assert(worker_batch_push(
                    worker, start, owner_count[owner]) == true);
        if (submitted)
            *submitted += owner_count[owner];
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
static int ensure_dispatcher_epoll(void)
{
    struct epoll_event event;

    if (dispatcher_epoll_fd >= 0)
        return OGS_OK;

    dispatcher_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (dispatcher_epoll_fd < 0) {
        ogs_error("epoll_create1() failed: %s", strerror(errno));
        return OGS_ERROR;
    }
    dispatcher_wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (dispatcher_wake_fd < 0) {
        ogs_error("eventfd() failed: %s", strerror(errno));
        close(dispatcher_epoll_fd);
        dispatcher_epoll_fd = -1;
        return OGS_ERROR;
    }

    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN;
    event.data.ptr = &dispatcher_wake_token;
    if (epoll_ctl(dispatcher_epoll_fd, EPOLL_CTL_ADD,
                dispatcher_wake_fd, &event) < 0) {
        ogs_error("epoll_ctl(wake-fd) failed: %s", strerror(errno));
        close(dispatcher_wake_fd);
        close(dispatcher_epoll_fd);
        dispatcher_wake_fd = -1;
        dispatcher_epoll_fd = -1;
        return OGS_ERROR;
    }
    return OGS_OK;
}

int upf_dataplane_memif_fd_update(
        memif_fd_event_t fde, void *private_ctx)
{
    struct epoll_event event;
    int operation = EPOLL_CTL_ADD;

    (void)private_ctx;
    if (ensure_dispatcher_epoll() != OGS_OK)
        return MEMIF_ERR_CB_FDUPDATE;
    if (fde.fd < 0)
        return MEMIF_ERR_BAD_FD;

    if (fde.type & MEMIF_FD_EVENT_DEL) {
        if (epoll_ctl(dispatcher_epoll_fd,
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
    if (epoll_ctl(dispatcher_epoll_fd,
                operation, fde.fd, &event) < 0) {
        ogs_error("epoll_ctl(%s fd:%d) failed: %s",
                operation == EPOLL_CTL_MOD ? "MOD" : "ADD",
                fde.fd, strerror(errno));
        return MEMIF_ERR_CB_FDUPDATE;
    }
    return MEMIF_ERR_SUCCESS;
}
#endif

void upf_dataplane_wake(void)
{
#if HAVE_LIBMEMIF
    uint64_t value = 1;

    if (dispatcher_wake_fd >= 0 &&
        write(dispatcher_wake_fd, &value, sizeof(value)) < 0 &&
        errno != EAGAIN)
        ogs_warn("UPF dispatcher wake failed: %s", strerror(errno));
#endif
}

void upf_dataplane_init(void)
{
    uint8_t i;

    pthread_rwlock_init(&rule_lock, NULL);
    pthread_mutex_init(&report_lock, NULL);
    memset(owned_sessions, 0, sizeof(owned_sessions));
    memset(workers, 0, sizeof(workers));
#if HAVE_LIBMEMIF
    for (i = 0; i < UPF_MAX_WORKERS; i++)
        workers[i].wake_fd = -1;
#else
    (void)i;
#endif
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
}

void upf_dataplane_final(void)
{
    ogs_assert(!dispatcher);
#if HAVE_LIBMEMIF
    if (dispatcher_wake_fd >= 0) {
        close(dispatcher_wake_fd);
        dispatcher_wake_fd = -1;
    }
    if (dispatcher_epoll_fd >= 0) {
        close(dispatcher_epoll_fd);
        dispatcher_epoll_fd = -1;
    }
#endif
    pthread_mutex_destroy(&report_lock);
    pthread_rwlock_destroy(&rule_lock);
}

int upf_dataplane_start(void)
{
    uint8_t i;
    size_t max_packet_size = upf_self()->n3.buffer_size;

    ogs_assert(!dispatcher);
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);

#if HAVE_LIBMEMIF
    if (ensure_dispatcher_epoll() != OGS_OK)
        return OGS_ERROR;
    epoll_waits = 0;
    epoll_events = 0;
    epoll_errors = 0;
    epoll_wakeups = 0;
#endif
    if (upf_self()->n6.buffer_size > max_packet_size)
        max_packet_size = upf_self()->n6.buffer_size;
    for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
        workers[i].id = i;
        workers[i].task_size = sizeof(upf_work_t) + max_packet_size;
        workers[i].task_pool = ogs_calloc(
                upf_self()->dataplane.worker_queue_size,
                workers[i].task_size);
        workers[i].batch_ring = ogs_calloc(
                upf_self()->dataplane.worker_queue_size,
                sizeof(upf_work_batch_t));
        if (!workers[i].task_pool || !workers[i].batch_ring)
            goto fail;
#if HAVE_LIBMEMIF
        workers[i].wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (workers[i].wake_fd < 0) {
            ogs_error("eventfd(worker %u) failed: %s",
                    i, strerror(errno));
            goto fail;
        }
#endif
        workers[i].write_seq = 0;
        __atomic_store_n(&workers[i].read_seq, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
                &workers[i].batch_write_seq, 0, __ATOMIC_RELEASE);
        __atomic_store_n(
                &workers[i].batch_read_seq, 0, __ATOMIC_RELEASE);
        workers[i].packets = 0;
        workers[i].drops = 0;
        workers[i].ring_full_batches = 0;
        workers[i].queue_high_water = 0;
        workers[i].batch_high_water = 0;
#if HAVE_LIBMEMIF
        __atomic_store_n(&workers[i].sleeping, 0, __ATOMIC_RELEASE);
        workers[i].wake_signals = 0;
        __atomic_store_n(&workers[i].wake_events, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&workers[i].sleeps, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&workers[i].busy_loops, 0, __ATOMIC_RELAXED);
#endif
        workers[i].thread = ogs_thread_create(session_worker_main, &workers[i]);
        if (!workers[i].thread)
            goto fail;
    }
    dispatcher = ogs_thread_create(dispatcher_main, NULL);
    if (!dispatcher)
        goto fail;
    return OGS_OK;

fail:
    upf_dataplane_stop();
    return OGS_ERROR;
}

void upf_dataplane_stop(void)
{
    uint8_t i;

    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    upf_dataplane_wake();
#if HAVE_LIBMEMIF
    for (i = 0; i < UPF_MAX_WORKERS; i++) {
        uint64_t value = 1;

        if (workers[i].wake_fd >= 0 &&
            write(workers[i].wake_fd, &value, sizeof(value)) < 0 &&
            errno != EAGAIN)
            ogs_warn("UPF worker %u stop wake failed: %s",
                    i, strerror(errno));
    }
#endif
    if (dispatcher) {
        ogs_thread_destroy(dispatcher);
        dispatcher = NULL;
    }
    for (i = 0; i < UPF_MAX_WORKERS; i++) {
        if (workers[i].thread) {
            ogs_thread_destroy(workers[i].thread);
            workers[i].thread = NULL;
        }
#if HAVE_LIBMEMIF
        if (workers[i].wake_fd >= 0) {
            close(workers[i].wake_fd);
            workers[i].wake_fd = -1;
        }
#endif
        if (workers[i].task_pool) {
            ogs_free(workers[i].task_pool);
            workers[i].task_pool = NULL;
        }
        if (workers[i].batch_ring) {
            ogs_free(workers[i].batch_ring);
            workers[i].batch_ring = NULL;
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
    uint8_t first = mixed % count;
    uint8_t second = (mixed >> 17) % count;
    unsigned int first_count;
    unsigned int second_count;
    uint8_t owner;

    if (second == first && count > 1)
        second = (second + 1) % count;
    first_count = __atomic_load_n(
            &owned_sessions[first], __ATOMIC_RELAXED);
    second_count = __atomic_load_n(
            &owned_sessions[second], __ATOMIC_RELAXED);
    owner = second_count < first_count ? second : first;
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
