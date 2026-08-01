/*
 * Copyright (C) 2026 by Open5GS contributors
 *
 * This file is part of Open5GS.
 */

#include "context.h"
#include "dataplane.h"
#include "gtp-path.h"
#include "n3-memif.h"
#include "n6-memif.h"
#include "rate-stats.h"

#if HAVE_LIBMEMIF
#include <libmemif.h>

#define UPF_MEMIF_MAX_BURST 256
#define UPF_MEMIF_MAX_QUEUES 16
#define UPF_MEMIF_MAX_RING_SIZE (1U << 14)

typedef struct {
    ogs_pkbuf_t *pkbuf;
    upf_rate_slot_t *rate_slot;
    uint32_t rate_octets;
} upf_n6_tx_packet_t;

typedef struct {
    uint64_t sequence;
    uint32_t generation;
    uint8_t done;
} upf_n6_lease_t;

typedef struct {
    bool pending;
    ogs_time_t pending_since;
    uint64_t interrupts;
    uint64_t bursts;
    uint64_t packets;
    uint64_t full_bursts;
    uint64_t budget_yields;
    uint64_t invalid_packets;
    uint64_t dispatch_drops;
    uint64_t rx_errors;
    uint64_t refill_errors;
    uint64_t refill_calls;
    uint64_t refill_packets;
    uint64_t completion_stale;
    uint64_t in_flight_max;
    uint64_t pending_max_us;
    uint64_t next_sequence;
    uint64_t refill_sequence;
    uint32_t generation;
    uint32_t in_flight;
    uint16_t burst_min;
    uint16_t burst_max;
    upf_n6_lease_t leases[UPF_MEMIF_MAX_RING_SIZE];
} upf_n6_rx_queue_t;

typedef struct {
    memif_socket_handle_t socket;
    memif_conn_handle_t connection;
    upf_n6_tx_packet_t tx_packets[UPF_MEMIF_MAX_BURST];
    uint16_t tx_count;
    uint64_t tx_send_requests;
    uint64_t tx_alloc_calls;
    uint64_t tx_alloc_requested;
    uint64_t tx_alloc_granted;
    uint64_t tx_drops;
    uint64_t tx_ring_full;
    uint64_t tx_alloc_failures;
    uint64_t tx_burst_calls;
    uint64_t tx_burst_requested;
    uint64_t tx_burst_sent;
    ogs_time_t last_tx_drop_log;
    bool tx_batch;
    bool connected;
} upf_n6_memif_state_t;

static upf_n6_memif_state_t state[16];
static upf_n6_rx_queue_t rx_queue[UPF_MEMIF_MAX_QUEUES];
static uint8_t rx_rr_qid;
#define self state[upf_dataplane_worker_id()]

static void reset_lease_queue(upf_n6_rx_queue_t *queue)
{
    __atomic_add_fetch(&queue->generation, 1, __ATOMIC_ACQ_REL);
    queue->next_sequence = 0;
    queue->refill_sequence = 0;
    queue->in_flight = 0;
}

static void reserve_lease(
        upf_n6_rx_queue_t *queue, uint16_t qid,
        upf_dataplane_packet_t *packet)
{
    uint32_t ring_size = 1U << upf_self()->n6.log2_ring_size;
    uint64_t sequence = queue->next_sequence++;
    upf_n6_lease_t *lease = &queue->leases[sequence % ring_size];

    __atomic_store_n(&lease->sequence, sequence, __ATOMIC_RELAXED);
    __atomic_store_n(
            &lease->generation, queue->generation, __ATOMIC_RELAXED);
    __atomic_store_n(&lease->done, 0, __ATOMIC_RELEASE);
    queue->in_flight++;
    if (queue->in_flight > queue->in_flight_max)
        queue->in_flight_max = queue->in_flight;

    packet->complete = upf_n6_memif_complete;
    packet->sequence = sequence;
    packet->generation = queue->generation;
    packet->qid = qid;
}

static int drain_queue_completions(uint16_t qid)
{
    upf_n6_rx_queue_t *queue = &rx_queue[qid];
    uint32_t ring_size = 1U << upf_self()->n6.log2_ring_size;
    uint16_t count = 0;
    int rv;

    while (queue->refill_sequence < queue->next_sequence &&
            count < UINT16_MAX) {
        upf_n6_lease_t *lease =
            &queue->leases[queue->refill_sequence % ring_size];

        if (!__atomic_load_n(&lease->done, __ATOMIC_ACQUIRE) ||
            __atomic_load_n(&lease->sequence, __ATOMIC_RELAXED) !=
                queue->refill_sequence ||
            __atomic_load_n(&lease->generation, __ATOMIC_RELAXED) !=
                queue->generation)
            break;
        count++;
        queue->refill_sequence++;
    }
    if (!count)
        return OGS_OK;

    rv = memif_refill_queue(state[qid].connection, qid, count, 0);
    if (rv != MEMIF_ERR_SUCCESS) {
        queue->refill_sequence -= count;
        queue->refill_errors++;
        ogs_error("N6 memif_refill_queue(qid:%u count:%u) failed: %s",
                qid, count, memif_strerror(rv));
        return OGS_ERROR;
    }
    queue->refill_calls++;
    queue->refill_packets += count;
    ogs_assert(queue->in_flight >= count);
    queue->in_flight -= count;
    return OGS_OK;
}

void upf_n6_memif_complete(
        uint16_t qid, uint64_t sequence, uint32_t generation)
{
    upf_n6_rx_queue_t *queue;
    upf_n6_lease_t *lease;
    uint32_t ring_size;

    if (qid >= upf_self()->n6.queues)
        return;
    queue = &rx_queue[qid];
    if (__atomic_load_n(&queue->generation, __ATOMIC_ACQUIRE) != generation) {
        __atomic_fetch_add(
                &queue->completion_stale, 1, __ATOMIC_RELAXED);
        return;
    }
    ring_size = 1U << upf_self()->n6.log2_ring_size;
    lease = &queue->leases[sequence % ring_size];
    if (__atomic_load_n(&lease->sequence, __ATOMIC_RELAXED) != sequence ||
        __atomic_load_n(&lease->generation, __ATOMIC_RELAXED) != generation ||
        __atomic_load_n(&queue->generation, __ATOMIC_ACQUIRE) != generation) {
        __atomic_fetch_add(
                &queue->completion_stale, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&lease->done, 1, __ATOMIC_RELEASE);
}

void upf_n6_memif_drain_completions(void)
{
    uint8_t qid;

    if (!state[0].connection)
        return;
    for (qid = 0; qid < upf_self()->n6.queues; qid++)
        drain_queue_completions(qid);
}

static void log_tx_drop(const char *reason, uint16_t count)
{
    ogs_time_t now = ogs_get_monotonic_time();

    ogs_assert(count);
    __atomic_fetch_add(&self.tx_drops, count, __ATOMIC_RELAXED);
    if (!self.last_tx_drop_log ||
        now - self.last_tx_drop_log >= ogs_time_from_sec(1)) {
        ogs_warn("[DROP] N6 memif TX failed [%s, total:%llu]",
                reason, (unsigned long long)__atomic_load_n(
                    &self.tx_drops, __ATOMIC_RELAXED));
        self.last_tx_drop_log = now;
    }
}

static int on_connect(memif_conn_handle_t connection, void *private_ctx)
{
    uint8_t i;
    int rv;

    (void)private_ctx;
    for (i = 0; i < upf_self()->n6.queues; i++) {
        rv = memif_refill_queue(connection, i, UINT16_MAX, 0);
        if (rv != MEMIF_ERR_SUCCESS) {
            ogs_error("memif_refill_queue(%u) failed: %s",
                    i, memif_strerror(rv));
            return rv;
        }
        state[i].connection = connection;
        state[i].socket = state[0].socket;
        state[i].connected = true;
        rx_queue[i].pending = false;
        rx_queue[i].pending_since = 0;
        reset_lease_queue(&rx_queue[i]);
    }
    ogs_info("N6 memif connected [socket:%s id:%u]",
            upf_self()->n6.socket_path, upf_self()->n6.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_disconnect(memif_conn_handle_t connection, void *private_ctx)
{
    uint8_t i;

    (void)connection;
    (void)private_ctx;
    for (i = 0; i < upf_self()->n6.queues; i++) {
        state[i].connected = false;
        rx_queue[i].pending = false;
        rx_queue[i].pending_since = 0;
        reset_lease_queue(&rx_queue[i]);
    }
    ogs_warn("N6 memif disconnected [socket:%s id:%u]",
            upf_self()->n6.socket_path, upf_self()->n6.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_interrupt(
        memif_conn_handle_t connection, void *private_ctx, uint16_t qid)
{
    upf_n6_rx_queue_t *queue;

    (void)connection;
    (void)private_ctx;
    if (qid >= upf_self()->n6.queues || qid >= UPF_MEMIF_MAX_QUEUES) {
        ogs_error("Invalid N6 memif RX qid:%u", qid);
        return MEMIF_ERR_QID;
    }

    queue = &rx_queue[qid];
    queue->interrupts++;
    if (!queue->pending) {
        queue->pending = true;
        queue->pending_since = ogs_get_monotonic_time();
    }
    return MEMIF_ERR_SUCCESS;
}

static int service_queue(
        uint16_t qid, uint32_t packet_budget, uint32_t time_budget_us)
{
    upf_n6_rx_queue_t *queue = &rx_queue[qid];
    memif_buffer_t buffers[UPF_MEMIF_MAX_BURST];
    upf_dataplane_packet_t packets[UPF_MEMIF_MAX_BURST];
    ogs_time_t started = ogs_get_monotonic_time();
    uint32_t processed = 0;
    uint16_t requested;
    uint16_t count;
    uint16_t packet_count;
    uint16_t submitted;
    bool more = true;
    int rv;
    int i;

    do {
        requested = upf_self()->n6.burst_size;
        if (requested > packet_budget - processed)
            requested = packet_budget - processed;
        if (requested > UPF_MEMIF_MAX_BURST)
            requested = UPF_MEMIF_MAX_BURST;
        count = 0;
        if (upf_self()->dataplane.session_workers &&
            drain_queue_completions(qid) != OGS_OK)
            return OGS_ERROR;
        rv = memif_rx_burst(
                state[qid].connection, qid, buffers, requested, &count);
        if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF) {
            queue->rx_errors++;
            queue->pending = false;
            queue->pending_since = 0;
            ogs_error("memif_rx_burst(qid:%u) failed: %s",
                    qid, memif_strerror(rv));
            return OGS_ERROR;
        }

        if (!queue->bursts || count < queue->burst_min)
            queue->burst_min = count;
        queue->bursts++;
        queue->packets += count;
        processed += count;
        if (count > queue->burst_max)
            queue->burst_max = count;
        if (count == requested)
            queue->full_bursts++;

        if (!upf_self()->dataplane.session_workers)
            upf_n3_memif_tx_batch_begin(count);
        packet_count = 0;
        for (i = 0; i < count; i++) {
            upf_dataplane_packet_t packet = { 0 };

            if (upf_self()->dataplane.session_workers)
                reserve_lease(queue, qid, &packet);
            if (buffers[i].flags & MEMIF_BUFFER_FLAG_NEXT) {
                ogs_error("[DROP] Chained N6 memif buffers are not supported");
                queue->invalid_packets++;
                if (packet.complete)
                    packet.complete(packet.qid,
                            packet.sequence, packet.generation);
                continue;
            }
            if (upf_self()->dataplane.session_workers) {
                packet.data = buffers[i].data;
                packet.len = buffers[i].len;
                if (packet.len < 1 ||
                    ((((const uint8_t *)packet.data)[0] >> 4) != 4 &&
                     (((const uint8_t *)packet.data)[0] >> 4) != 6))
                    packet.force_copy = true;
                packets[packet_count] = packet;
                packet_count++;
            } else if (upf_gtp_handle_n6_data(
                        buffers[i].data, buffers[i].len) != OGS_OK) {
                ogs_error("[DROP] Invalid packet received from N6 memif");
                queue->invalid_packets++;
            }
        }
        if (upf_self()->dataplane.session_workers && packet_count) {
            submitted = 0;
            if (upf_dataplane_submit_n6_batch(
                        packets, packet_count, &submitted) != OGS_OK)
                ogs_debug("[DROP] N6 memif batch was not fully dispatched");
            queue->dispatch_drops += packet_count - submitted;
        } else if (!upf_self()->dataplane.session_workers) {
            upf_n3_memif_tx_batch_flush();
        }

        if (upf_self()->dataplane.session_workers) {
            if (drain_queue_completions(qid) != OGS_OK)
                return OGS_ERROR;
        } else {
            rv = memif_refill_queue(state[qid].connection, qid, count, 0);
            if (rv != MEMIF_ERR_SUCCESS) {
                queue->refill_errors++;
                queue->pending = false;
                queue->pending_since = 0;
                ogs_error("N6 memif_refill_queue(qid:%u count:%u) failed: %s",
                        qid, count, memif_strerror(rv));
                return OGS_ERROR;
            }
        }

        more = count == requested;
    } while (more && processed < packet_budget &&
            ogs_get_monotonic_time() - started < (ogs_time_t)time_budget_us);

    if (more) {
        ogs_time_t pending_us =
            ogs_get_monotonic_time() - queue->pending_since;

        queue->budget_yields++;
        if (pending_us > (ogs_time_t)queue->pending_max_us)
            queue->pending_max_us = pending_us;
    } else {
        ogs_time_t pending_us =
            ogs_get_monotonic_time() - queue->pending_since;

        if (pending_us > (ogs_time_t)queue->pending_max_us)
            queue->pending_max_us = pending_us;
        queue->pending = false;
        queue->pending_since = 0;
    }
    return OGS_OK;
}

int upf_n6_memif_open(void)
{
    memif_socket_args_t socket_args;
    memif_conn_args_t connection_args;
    int rv;

    memset(state, 0, sizeof(state));
    memset(rx_queue, 0, sizeof(rx_queue));
    rx_rr_qid = 0;

    memset(&socket_args, 0, sizeof(socket_args));
    ogs_cpystrn(socket_args.path, upf_self()->n6.socket_path,
            sizeof(socket_args.path));
    ogs_cpystrn(socket_args.app_name, "open5gs-upf",
            sizeof(socket_args.app_name));
    socket_args.connection_request_timer.it_value.tv_sec = 1;
    socket_args.connection_request_timer.it_interval.tv_sec = 1;
    socket_args.on_control_fd_update = upf_dataplane_n6_memif_fd_update;

    rv = memif_create_socket(&self.socket, &socket_args, NULL);
    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("memif_create_socket(%s) failed: %s",
                socket_args.path, memif_strerror(rv));
        return OGS_ERROR;
    }

    memset(&connection_args, 0, sizeof(connection_args));
    connection_args.socket = self.socket;
    connection_args.interface_id = upf_self()->n6.interface_id;
    connection_args.buffer_size = upf_self()->n6.buffer_size;
    connection_args.log2_ring_size = upf_self()->n6.log2_ring_size;
    connection_args.num_s2m_rings = upf_self()->n6.queues;
    connection_args.num_m2s_rings = upf_self()->n6.queues;
    connection_args.is_master = 0; /* Open5GS is the memif slave/client. */
    connection_args.mode = MEMIF_INTERFACE_MODE_IP;
    ogs_cpystrn((char *)connection_args.interface_name, "open5gs-n6",
            sizeof(connection_args.interface_name));

    rv = memif_create(&self.connection, &connection_args,
            on_connect, on_disconnect, on_interrupt, NULL);
    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("memif_create(id:%u) failed: %s",
                connection_args.interface_id, memif_strerror(rv));
        upf_n6_memif_close();
        return OGS_ERROR;
    }

    ogs_info("N6 memif initialized [socket:%s id:%u buffer:%u ring:%u]",
            upf_self()->n6.socket_path, upf_self()->n6.interface_id,
            upf_self()->n6.buffer_size,
            1U << upf_self()->n6.log2_ring_size);
    return OGS_OK;
}

void upf_n6_memif_close(void)
{
    upf_n6_memif_tx_batch_flush();
    self.connected = false;
    if (self.connection)
        memif_delete(&self.connection);
    if (self.socket)
        memif_delete_socket(&self.socket);

}

bool upf_n6_memif_has_pending(void)
{
    uint8_t i;

    for (i = 0; i < upf_self()->n6.queues; i++)
        if (rx_queue[i].pending || rx_queue[i].in_flight)
            return true;
    return false;
}

int upf_n6_memif_service(
        uint32_t packet_budget, uint32_t time_budget_us)
{
    uint8_t queues = upf_self()->n6.queues;
    uint8_t offset;

    if (upf_self()->dataplane.session_workers)
        upf_n6_memif_drain_completions();
    for (offset = 0; offset < queues; offset++) {
        uint8_t qid = (rx_rr_qid + offset) % queues;

        if (!rx_queue[qid].pending)
            continue;
        rx_rr_qid = (qid + 1) % queues;
        return service_queue(qid, packet_budget, time_budget_us);
    }
    return OGS_OK;
}

void upf_n6_memif_log_stats(void)
{
    uint8_t i;

    for (i = 0; i < upf_self()->n6.queues; i++) {
        upf_n6_rx_queue_t *queue = &rx_queue[i];
        uint64_t average = queue->bursts ?
            queue->packets / queue->bursts : 0;
        ogs_time_t pending_us = queue->pending ?
            ogs_get_monotonic_time() - queue->pending_since : 0;

        ogs_info("N6 memif RX qid:%u stats "
                "[interrupt:%llu burst:%llu packets:%llu "
                "burst-size(min/avg/max):%u/%llu/%u full:%llu "
                "budget-yield:%llu pending:%s pending-us:%lld max-us:%llu "
                "invalid:%llu dispatch-drop:%llu rx-error:%llu "
                "refill-error:%llu refill-call:%llu refill-packets:%llu "
                "in-flight:%u in-flight-max:%llu stale:%llu]",
                i, (unsigned long long)queue->interrupts,
                (unsigned long long)queue->bursts,
                (unsigned long long)queue->packets,
                queue->burst_min, (unsigned long long)average,
                queue->burst_max, (unsigned long long)queue->full_bursts,
                (unsigned long long)queue->budget_yields,
                queue->pending ? "yes" : "no", (long long)pending_us,
                (unsigned long long)queue->pending_max_us,
                (unsigned long long)queue->invalid_packets,
                (unsigned long long)queue->dispatch_drops,
                (unsigned long long)queue->rx_errors,
                (unsigned long long)queue->refill_errors,
                (unsigned long long)queue->refill_calls,
                (unsigned long long)queue->refill_packets,
                queue->in_flight,
                (unsigned long long)queue->in_flight_max,
                (unsigned long long)queue->completion_stale);
        ogs_info("N6 memif TX qid:%u stats "
                "[send-request:%llu alloc-call:%llu "
                "alloc-request:%llu alloc-granted:%llu "
                "alloc-short:%llu alloc-fail:%llu "
                "tx-call:%llu tx-request:%llu tx-sent:%llu drop:%llu]",
                i,
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_send_requests, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_alloc_calls, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_alloc_requested, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_alloc_granted, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_ring_full, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_alloc_failures, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_burst_calls, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_burst_requested, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_burst_sent, __ATOMIC_RELAXED),
                (unsigned long long)__atomic_load_n(
                    &state[i].tx_drops, __ATOMIC_RELAXED));
    }
}

void upf_n6_memif_tx_batch_begin(void)
{
    ogs_assert(self.tx_count == 0);
    self.tx_batch = true;
}

void upf_n6_memif_tx_batch_flush(void)
{
    memif_buffer_t buffers[UPF_MEMIF_MAX_BURST];
    uint16_t allocated = 0;
    uint16_t sent = 0;
    uint16_t max_len = 0;
    uint16_t i;
    int rv;

    self.tx_batch = false;
    if (!self.tx_count)
        return;

    if (!self.connected) {
        log_tx_drop("disconnected", self.tx_count);
        goto cleanup;
    }

    for (i = 0; i < self.tx_count; i++) {
        if (self.tx_packets[i].pkbuf->len > max_len)
            max_len = self.tx_packets[i].pkbuf->len;
    }

    __atomic_fetch_add(&self.tx_alloc_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
            &self.tx_alloc_requested, self.tx_count, __ATOMIC_RELAXED);
    rv = memif_buffer_alloc(self.connection, upf_dataplane_worker_id(),
            buffers, self.tx_count,
            &allocated, max_len);
    __atomic_fetch_add(
            &self.tx_alloc_granted, allocated, __ATOMIC_RELAXED);
    if (allocated < self.tx_count)
        __atomic_fetch_add(&self.tx_ring_full,
                self.tx_count - allocated, __ATOMIC_RELAXED);
    if ((rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF_RING &&
            rv != MEMIF_ERR_NOBUF) || !allocated) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_buffer_alloc() failed: %s", memif_strerror(rv));
        __atomic_fetch_add(&self.tx_alloc_failures, 1, __ATOMIC_RELAXED);
        log_tx_drop(memif_strerror(rv), self.tx_count);
        goto cleanup;
    }

    for (i = 0; i < allocated; i++) {
        memcpy(buffers[i].data, self.tx_packets[i].pkbuf->data,
                self.tx_packets[i].pkbuf->len);
        buffers[i].len = self.tx_packets[i].pkbuf->len;
    }

    __atomic_fetch_add(&self.tx_burst_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
            &self.tx_burst_requested, allocated, __ATOMIC_RELAXED);
    rv = memif_tx_burst(self.connection, upf_dataplane_worker_id(),
            buffers, allocated, &sent);
    __atomic_fetch_add(&self.tx_burst_sent, sent, __ATOMIC_RELAXED);
    for (i = 0; i < sent; i++)
        upf_rate_stats_record(self.tx_packets[i].rate_slot,
                self.tx_packets[i].rate_octets);
    if (sent != allocated) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_tx_burst() failed: %s", memif_strerror(rv));
        log_tx_drop(memif_strerror(rv), allocated - sent);
    } else if (rv != MEMIF_ERR_SUCCESS)
        ogs_warn("N6 memif TX notification failed: %s",
                memif_strerror(rv));
    if (allocated != self.tx_count)
        log_tx_drop("partial batch allocation", self.tx_count - allocated);

cleanup:
    for (i = 0; i < self.tx_count; i++)
        ogs_pkbuf_free(self.tx_packets[i].pkbuf);
    self.tx_count = 0;
}

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf,
        upf_rate_slot_t *rate_slot, uint32_t rate_octets)
{
    bool standalone = !self.tx_batch;

    ogs_assert(pkbuf);

    __atomic_fetch_add(&self.tx_send_requests, 1, __ATOMIC_RELAXED);
    if (pkbuf->len > upf_self()->n6.buffer_size || pkbuf->len > UINT16_MAX) {
        log_tx_drop("packet too large", 1);
        return OGS_ERROR;
    }

    if (self.tx_count == UPF_MEMIF_MAX_BURST) {
        upf_n6_memif_tx_batch_flush();
        self.tx_batch = true;
    }

    ogs_pkbuf_ref((ogs_pkbuf_t *)pkbuf);
    self.tx_packets[self.tx_count].pkbuf = (ogs_pkbuf_t *)pkbuf;
    self.tx_packets[self.tx_count].rate_slot = rate_slot;
    self.tx_packets[self.tx_count].rate_octets = rate_octets;
    self.tx_count++;

    if (standalone)
        upf_n6_memif_tx_batch_flush();
    return OGS_OK;
}

#else /* HAVE_LIBMEMIF */

int upf_n6_memif_open(void)
{
    ogs_error("N6 memif backend requested, but Open5GS lacks libmemif");
    return OGS_ERROR;
}

void upf_n6_memif_close(void)
{
}

bool upf_n6_memif_has_pending(void)
{
    return false;
}

int upf_n6_memif_service(
        uint32_t packet_budget, uint32_t time_budget_us)
{
    (void)packet_budget;
    (void)time_budget_us;
    return OGS_OK;
}

void upf_n6_memif_log_stats(void)
{
}

void upf_n6_memif_tx_batch_begin(void)
{
}

void upf_n6_memif_tx_batch_flush(void)
{
}

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf,
        upf_rate_slot_t *rate_slot, uint32_t rate_octets)
{
    (void)pkbuf;
    (void)rate_slot;
    (void)rate_octets;
    return OGS_ERROR;
}

#endif /* HAVE_LIBMEMIF */
