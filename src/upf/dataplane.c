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

#define UPF_MAX_WORKERS 16

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
    uint8_t id;
    ogs_thread_t *thread;
    ogs_queue_t *queue;
    ogs_queue_t *free_queue;
    void *task_pool;
    size_t task_size;
    uint64_t packets;
    uint64_t drops;
} upf_worker_t;

static ogs_thread_t *dispatcher;
static upf_worker_t workers[UPF_MAX_WORKERS];
static pthread_rwlock_t rule_lock;
static pthread_mutex_t report_lock;
static unsigned int owned_sessions[UPF_MAX_WORKERS];
static int stopping;
static __thread uint8_t tls_worker_id;

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

static void session_worker_main(void *data)
{
    upf_worker_t *worker = data;
    upf_work_t *batch[256];

    tls_worker_id = worker->id;
    pin_current_thread_when_ready(worker->id, "session-worker");
    ogs_info("UPF session worker %u started", worker->id);
    while (!is_stopping()) {
        upf_work_t *work = NULL;
        unsigned int count = 0;
        unsigned int n6_count = 0;
        unsigned int i;
        int rv = ogs_queue_pop(worker->queue, (void **)&work);

        if (rv != OGS_OK || !work)
            break;

        batch[count++] = work;
        while (count < 256 &&
                ogs_queue_trypop(worker->queue,
                    (void **)&batch[count]) == OGS_OK)
            count++;
        for (i = 0; i < count; i++)
            if (batch[i]->type == UPF_WORK_N6)
                n6_count++;

        upf_dataplane_read_lock();
        upf_n6_memif_tx_batch_begin();
        upf_n3_memif_tx_batch_begin(n6_count);
        for (i = 0; i < count; i++) {
            work = batch[i];
            if (work->type == UPF_WORK_N3)
                upf_gtp_handle_n3_data(
                        work->data, work->len, &work->from);
            else
                upf_gtp_handle_n6_data(work->data, work->len);
        }
        upf_n6_memif_tx_batch_flush();
        upf_n3_memif_tx_batch_flush();
        upf_dataplane_read_unlock();
        worker->packets += count;
        for (i = 0; i < count; i++)
            ogs_assert(ogs_queue_trypush(
                        worker->free_queue, batch[i]) == OGS_OK);
    }
    ogs_info("UPF session worker %u stopped [packets:%llu drops:%llu]",
            worker->id, (unsigned long long)worker->packets,
            (unsigned long long)worker->drops);
}

static void dispatcher_main(void *data)
{
    int n3_rv;
    int n6_rv;

    tls_worker_id = 0;
    pin_current_thread_when_ready(
            upf_self()->dataplane.worker_count, "memif-dispatcher");
    ogs_info("UPF memif dispatcher started");
    while (!is_stopping()) {
        if (!upf_self()->dataplane.session_workers)
            upf_dataplane_read_lock();
        n3_rv = upf_n3_memif_poll();
        n6_rv = upf_n6_memif_poll();
        if (!upf_self()->dataplane.session_workers)
            upf_dataplane_read_unlock();
        if (n3_rv == OGS_DONE && n6_rv == OGS_DONE)
            ogs_usleep(1000);
    }
    ogs_info("UPF memif dispatcher stopped");
}

static uint8_t owner_from_n3(const void *data, size_t len)
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

    upf_dataplane_read_lock();
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
    upf_dataplane_read_unlock();
    return (uint8_t)teid;
}

static uint8_t owner_from_n6(const void *data, size_t len)
{
    const uint8_t *p = data;
    upf_sess_t *sess = NULL;
    uint8_t owner = 0;

    upf_dataplane_read_lock();
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
    upf_dataplane_read_unlock();
    return owner;
}

static int submit_work(upf_work_type_t type, uint8_t owner,
        const void *data, size_t len, const ogs_sockaddr_t *from)
{
    upf_work_t *work;
    int rv;

    if (!data || !len || owner >= upf_self()->dataplane.worker_count)
        return OGS_ERROR;
    if (sizeof(*work) + len > workers[owner].task_size)
        return OGS_ERROR;
    rv = ogs_queue_trypop(workers[owner].free_queue, (void **)&work);
    if (rv != OGS_OK) {
        workers[owner].drops++;
        return OGS_ERROR;
    }
    memset(work, 0, sizeof(*work));
    work->type = type;
    work->len = len;
    if (from)
        memcpy(&work->from, from, sizeof(*from));
    memcpy(work->data, data, len);

    rv = ogs_queue_trypush(workers[owner].queue, work);
    if (rv != OGS_OK) {
        workers[owner].drops++;
        ogs_assert(ogs_queue_trypush(
                    workers[owner].free_queue, work) == OGS_OK);
        return OGS_ERROR;
    }
    return OGS_OK;
}

int upf_dataplane_submit_n3(
        const void *data, size_t len, const ogs_sockaddr_t *from)
{
    return submit_work(UPF_WORK_N3, owner_from_n3(data, len),
            data, len, from);
}

int upf_dataplane_submit_n6(const void *data, size_t len)
{
    return submit_work(UPF_WORK_N6, owner_from_n6(data, len),
            data, len, NULL);
}

void upf_dataplane_init(void)
{
    pthread_rwlock_init(&rule_lock, NULL);
    pthread_mutex_init(&report_lock, NULL);
    memset(owned_sessions, 0, sizeof(owned_sessions));
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
}

void upf_dataplane_final(void)
{
    ogs_assert(!dispatcher);
    pthread_mutex_destroy(&report_lock);
    pthread_rwlock_destroy(&rule_lock);
}

int upf_dataplane_start(void)
{
    uint8_t i;
    uint32_t j;
    size_t max_packet_size = upf_self()->n3.buffer_size;

    ogs_assert(!dispatcher);
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);

    if (upf_self()->n6.buffer_size > max_packet_size)
        max_packet_size = upf_self()->n6.buffer_size;
    for (i = 0; i < upf_self()->dataplane.worker_count; i++) {
        workers[i].id = i;
        workers[i].task_size = sizeof(upf_work_t) + max_packet_size;
        workers[i].queue =
            ogs_queue_create(upf_self()->dataplane.worker_queue_size);
        workers[i].free_queue =
            ogs_queue_create(upf_self()->dataplane.worker_queue_size);
        workers[i].task_pool = ogs_calloc(
                upf_self()->dataplane.worker_queue_size,
                workers[i].task_size);
        if (!workers[i].queue || !workers[i].free_queue ||
            !workers[i].task_pool)
            goto fail;
        for (j = 0; j < upf_self()->dataplane.worker_queue_size; j++)
            ogs_assert(ogs_queue_trypush(workers[i].free_queue,
                        (uint8_t *)workers[i].task_pool +
                        j * workers[i].task_size) == OGS_OK);
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
    upf_n3_memif_cancel_poll();
    upf_n6_memif_cancel_poll();
    if (dispatcher) {
        ogs_thread_destroy(dispatcher);
        dispatcher = NULL;
    }
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
