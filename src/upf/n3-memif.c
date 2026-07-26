/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "context.h"
#include "dataplane.h"
#include "gtp-path.h"
#include "n3-memif.h"
#include "n6-memif.h"

#include <netinet/ip.h>
#include <netinet/udp.h>

#if HAVE_LIBMEMIF
#include <libmemif.h>

#define UPF_MEMIF_MAX_BURST 256
#define UPF_MEMIF_TX_CHUNK 64
#define UPF_GTPU_PORT 2152
#define UPF_MEMIF_MAX_QUEUES 16

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
    uint64_t pending_max_us;
    uint16_t burst_min;
    uint16_t burst_max;
} upf_n3_rx_queue_t;

typedef struct upf_n3_tx_packet_s {
    ogs_pkbuf_t *pkbuf;
    ogs_sockaddr_t to;
    ogs_pkbuf_t direct_pkbuf;
    bool ready;
} upf_n3_tx_packet_t;

typedef struct {
    memif_socket_handle_t socket;
    memif_conn_handle_t connection;
    struct in_addr local_addr;
    upf_n3_tx_packet_t tx_packets[UPF_MEMIF_MAX_BURST];
    memif_buffer_t tx_buffers[UPF_MEMIF_MAX_BURST];
    uint16_t tx_count;
    uint16_t tx_allocated;
    uint16_t tx_batch_remaining;
    uint16_t ip_id;
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
} upf_n3_memif_state_t;

static upf_n3_memif_state_t state[16];
static upf_n3_rx_queue_t rx_queue[UPF_MEMIF_MAX_QUEUES];
static uint8_t rx_rr_qid;
#define self state[upf_dataplane_worker_id()]

static void release_direct_pkbuf(ogs_pkbuf_t *pkbuf)
{
    /* Storage belongs to the fixed per-batch TX entry and memif ring. */
    ogs_assert(pkbuf);
}

static void log_tx_drop(const char *reason, uint16_t count)
{
    ogs_time_t now = ogs_get_monotonic_time();

    ogs_assert(reason);
    ogs_assert(count);

    __atomic_fetch_add(&self.tx_drops, count, __ATOMIC_RELAXED);
    if (!self.last_tx_drop_log ||
        now - self.last_tx_drop_log >= ogs_time_from_sec(1)) {
        ogs_warn("[DROP] N3 memif TX failed "
                "[%s dropped:%u total:%llu]",
                reason, count, (unsigned long long)__atomic_load_n(
                    &self.tx_drops, __ATOMIC_RELAXED));
        self.last_tx_drop_log = now;
    }
}

static uint32_t checksum_add(uint32_t sum, const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len >= 2) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)p[0] << 8;
    return sum;
}

static uint16_t checksum_finish(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ipv4_checksum(const void *data, size_t len)
{
    return checksum_finish(checksum_add(0, data, len));
}

static uint16_t udp_checksum(const struct ip *ip_h,
        const struct udphdr *udp_h, uint16_t udp_len)
{
    uint32_t sum = 0;
    uint8_t pseudo[4] = { 0, IPPROTO_UDP,
        (uint8_t)(udp_len >> 8), (uint8_t)udp_len };

    sum = checksum_add(sum, &ip_h->ip_src, sizeof(ip_h->ip_src));
    sum = checksum_add(sum, &ip_h->ip_dst, sizeof(ip_h->ip_dst));
    sum = checksum_add(sum, pseudo, sizeof(pseudo));
    sum = checksum_add(sum, udp_h, udp_len);
    return checksum_finish(sum);
}

static int parse_raw_ipv4(const void *data, uint16_t len,
        upf_dataplane_packet_t *packet)
{
    const struct ip *ip_h = data;
    const struct udphdr *udp_h;
    ogs_sockaddr_t from;
    uint16_t ip_hlen;
    uint16_t ip_len;
    uint16_t ip_off;
    uint16_t udp_len;

    if (!data || len < sizeof(struct ip))
        return OGS_ERROR;
    if (ip_h->ip_v != 4 || ip_h->ip_hl < 5)
        return OGS_ERROR;

    ip_hlen = (uint16_t)ip_h->ip_hl << 2;
    if (ip_hlen > len)
        return OGS_ERROR;
    ip_len = be16toh(ip_h->ip_len);
    if (ip_len < ip_hlen + sizeof(struct udphdr) || ip_len > len)
        return OGS_ERROR;
    if (ipv4_checksum(ip_h, ip_hlen) != 0)
        return OGS_ERROR;

    ip_off = be16toh(ip_h->ip_off);
    if (ip_off & (IP_MF | IP_OFFMASK)) {
        ogs_warn("[DROP] Fragmented IPv4 packet on N3 memif");
        return OGS_ERROR;
    }
    if (ip_h->ip_p != IPPROTO_UDP ||
        ip_h->ip_dst.s_addr != self.local_addr.s_addr)
        return OGS_ERROR;

    udp_h = (const struct udphdr *)((const uint8_t *)data + ip_hlen);
    udp_len = be16toh(udp_h->uh_ulen);
    if (udp_len < sizeof(struct udphdr) ||
        udp_len != ip_len - ip_hlen ||
        be16toh(udp_h->uh_dport) != UPF_GTPU_PORT)
        return OGS_ERROR;
    if (udp_h->uh_sum && udp_checksum(ip_h, udp_h, udp_len) != 0)
        return OGS_ERROR;

    memset(&from, 0, sizeof(from));
    from.ogs_sa_family = AF_INET;
    from.sin.sin_addr = ip_h->ip_src;
    from.ogs_sin_port = udp_h->uh_sport;

    packet->data = (const uint8_t *)udp_h + sizeof(*udp_h);
    packet->len = udp_len - sizeof(*udp_h);
    memcpy(&packet->from, &from, sizeof(packet->from));
    return OGS_OK;
}

static int on_connect(memif_conn_handle_t connection, void *private_ctx)
{
    uint8_t i;
    int rv;

    (void)private_ctx;
    for (i = 0; i < upf_self()->n3.queues; i++) {
        rv = memif_refill_queue(connection, i, UINT16_MAX, 0);
        if (rv != MEMIF_ERR_SUCCESS) {
            ogs_error("N3 memif_refill_queue(%u) failed: %s",
                    i, memif_strerror(rv));
            return rv;
        }
        state[i].connection = connection;
        state[i].socket = state[0].socket;
        state[i].local_addr = state[0].local_addr;
        state[i].connected = true;
        rx_queue[i].pending = false;
        rx_queue[i].pending_since = 0;
    }
    self.tx_count = 0;
    self.tx_allocated = 0;
    self.tx_batch_remaining = 0;
    self.connected = true;
    ogs_info("N3 memif connected [socket:%s id:%u]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_disconnect(memif_conn_handle_t connection, void *private_ctx)
{
    uint8_t i;

    (void)connection;
    (void)private_ctx;
    for (i = 0; i < upf_self()->n3.queues; i++) {
        state[i].connected = false;
        rx_queue[i].pending = false;
        rx_queue[i].pending_since = 0;
    }
    upf_n3_memif_tx_batch_flush();
    self.tx_allocated = 0;
    self.tx_batch_remaining = 0;
    ogs_warn("N3 memif disconnected [socket:%s id:%u]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_interrupt(
        memif_conn_handle_t connection, void *private_ctx, uint16_t qid)
{
    upf_n3_rx_queue_t *queue;

    (void)connection;
    (void)private_ctx;
    if (qid >= upf_self()->n3.queues || qid >= UPF_MEMIF_MAX_QUEUES) {
        ogs_error("Invalid N3 memif RX qid:%u", qid);
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
    upf_n3_rx_queue_t *queue = &rx_queue[qid];
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
        requested = upf_self()->n3.burst_size;
        if (requested > packet_budget - processed)
            requested = packet_budget - processed;
        if (requested > UPF_MEMIF_MAX_BURST)
            requested = UPF_MEMIF_MAX_BURST;
        count = 0;
        rv = memif_rx_burst(
                state[qid].connection, qid, buffers, requested, &count);
        if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF) {
            queue->rx_errors++;
            queue->pending = false;
            queue->pending_since = 0;
            ogs_error("N3 memif_rx_burst(qid:%u) failed: %s",
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
            upf_n6_memif_tx_batch_begin();
        packet_count = 0;
        for (i = 0; i < count; i++) {
            if (buffers[i].flags & MEMIF_BUFFER_FLAG_NEXT) {
                ogs_error("[DROP] Chained N3 memif buffers are unsupported");
                queue->invalid_packets++;
                continue;
            }
            if (parse_raw_ipv4(buffers[i].data, buffers[i].len,
                        &packets[packet_count]) != OGS_OK) {
                ogs_debug("[DROP] Invalid packet received from N3 memif");
                queue->invalid_packets++;
                continue;
            }
            if (upf_self()->dataplane.session_workers)
                packet_count++;
            else if (upf_gtp_handle_n3_data(packets[0].data,
                        packets[0].len, &packets[0].from) != OGS_OK)
                ogs_debug("[DROP] Invalid packet received from N3 memif");
        }
        if (upf_self()->dataplane.session_workers && packet_count) {
            submitted = 0;
            if (upf_dataplane_submit_n3_batch(
                        packets, packet_count, &submitted) != OGS_OK)
                ogs_debug("[DROP] N3 memif batch was not fully dispatched");
            queue->dispatch_drops += packet_count - submitted;
        } else if (!upf_self()->dataplane.session_workers) {
            upf_n6_memif_tx_batch_flush();
        }

        rv = memif_refill_queue(state[qid].connection, qid, count, 0);
        if (rv != MEMIF_ERR_SUCCESS) {
            queue->refill_errors++;
            queue->pending = false;
            queue->pending_since = 0;
            ogs_error("N3 memif_refill_queue(qid:%u count:%u) failed: %s",
                    qid, count, memif_strerror(rv));
            return OGS_ERROR;
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

int upf_n3_memif_open(void)
{
    memif_socket_args_t socket_args;
    memif_conn_args_t connection_args;
    ogs_sockaddr_t local;
    int rv;

    memset(state, 0, sizeof(state));
    memset(rx_queue, 0, sizeof(rx_queue));
    rx_rr_qid = 0;

    memset(&local, 0, sizeof(local));
    if (ogs_inet_pton(AF_INET, upf_self()->n3.local_address, &local) != OGS_OK)
        return OGS_ERROR;
    self.local_addr = local.sin.sin_addr;

    memset(&socket_args, 0, sizeof(socket_args));
    ogs_cpystrn(socket_args.path, upf_self()->n3.socket_path,
            sizeof(socket_args.path));
    ogs_cpystrn(socket_args.app_name, "open5gs-upf-n3",
            sizeof(socket_args.app_name));
    socket_args.connection_request_timer.it_value.tv_sec = 1;
    socket_args.connection_request_timer.it_interval.tv_sec = 1;
    socket_args.on_control_fd_update = upf_dataplane_memif_fd_update;

    rv = memif_create_socket(&self.socket, &socket_args, NULL);
    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("N3 memif_create_socket(%s) failed: %s",
                socket_args.path, memif_strerror(rv));
        return OGS_ERROR;
    }

    memset(&connection_args, 0, sizeof(connection_args));
    connection_args.socket = self.socket;
    connection_args.interface_id = upf_self()->n3.interface_id;
    connection_args.buffer_size = upf_self()->n3.buffer_size;
    connection_args.log2_ring_size = upf_self()->n3.log2_ring_size;
    connection_args.num_s2m_rings = upf_self()->n3.queues;
    connection_args.num_m2s_rings = upf_self()->n3.queues;
    connection_args.is_master = 0;
    connection_args.mode = MEMIF_INTERFACE_MODE_IP;
    ogs_cpystrn((char *)connection_args.interface_name, "open5gs-n3",
            sizeof(connection_args.interface_name));

    rv = memif_create(&self.connection, &connection_args,
            on_connect, on_disconnect, on_interrupt, NULL);
    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("N3 memif_create(id:%u) failed: %s",
                connection_args.interface_id, memif_strerror(rv));
        upf_n3_memif_close();
        return OGS_ERROR;
    }

    ogs_gtp_set_user_plane_send_cb(upf_n3_memif_send_gtpu);
    ogs_info("N3 memif initialized [socket:%s id:%u address:%s ring:%u udp-checksum:%s]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id,
            upf_self()->n3.local_address,
            1U << upf_self()->n3.log2_ring_size,
            upf_self()->n3.udp_checksum ? "on" : "off");
    return OGS_OK;
}

void upf_n3_memif_close(void)
{
    upf_n3_memif_tx_batch_flush();
    ogs_gtp_set_user_plane_send_cb(NULL);
    self.connected = false;
    if (self.connection)
        memif_delete(&self.connection);
    if (self.socket)
        memif_delete_socket(&self.socket);

}

bool upf_n3_memif_has_pending(void)
{
    uint8_t i;

    for (i = 0; i < upf_self()->n3.queues; i++)
        if (rx_queue[i].pending)
            return true;
    return false;
}

int upf_n3_memif_service(
        uint32_t packet_budget, uint32_t time_budget_us)
{
    uint8_t queues = upf_self()->n3.queues;
    uint8_t offset;

    for (offset = 0; offset < queues; offset++) {
        uint8_t qid = (rx_rr_qid + offset) % queues;

        if (!rx_queue[qid].pending)
            continue;
        rx_rr_qid = (qid + 1) % queues;
        return service_queue(qid, packet_budget, time_budget_us);
    }
    return OGS_OK;
}

void upf_n3_memif_log_stats(void)
{
    uint8_t i;

    for (i = 0; i < upf_self()->n3.queues; i++) {
        upf_n3_rx_queue_t *queue = &rx_queue[i];
        uint64_t average = queue->bursts ?
            queue->packets / queue->bursts : 0;
        ogs_time_t pending_us = queue->pending ?
            ogs_get_monotonic_time() - queue->pending_since : 0;

        ogs_info("N3 memif RX qid:%u stats "
                "[interrupt:%llu burst:%llu packets:%llu "
                "burst-size(min/avg/max):%u/%llu/%u full:%llu "
                "budget-yield:%llu pending:%s pending-us:%lld max-us:%llu "
                "invalid:%llu dispatch-drop:%llu rx-error:%llu "
                "refill-error:%llu]",
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
                (unsigned long long)queue->refill_errors);
        ogs_info("N3 memif TX qid:%u stats "
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

static bool preallocate_tx_buffers(uint16_t count)
{
    uint16_t allocated = 0;
    int rv;

    ogs_assert(self.tx_count == 0);
    ogs_assert(self.tx_allocated == 0);

    if (!self.connected || !count)
        return false;

    __atomic_fetch_add(&self.tx_alloc_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
            &self.tx_alloc_requested, count, __ATOMIC_RELAXED);
    rv = memif_buffer_alloc(self.connection, upf_dataplane_worker_id(),
            self.tx_buffers, count,
            &allocated, upf_self()->n3.buffer_size);
    __atomic_fetch_add(
            &self.tx_alloc_granted, allocated, __ATOMIC_RELAXED);
    if (allocated < count)
        __atomic_fetch_add(&self.tx_ring_full,
                count - allocated, __ATOMIC_RELAXED);
    if ((rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF_RING &&
            rv != MEMIF_ERR_NOBUF) || !allocated) {
        __atomic_fetch_add(&self.tx_alloc_failures, 1, __ATOMIC_RELAXED);
        return false;
    }

    self.tx_allocated = allocated;
    return true;
}

static bool ensure_tx_buffer(void)
{
    bool batch = self.tx_batch;
    uint16_t count = 1;

    if (self.tx_count < self.tx_allocated)
        return true;

    if (self.tx_count) {
        upf_n3_memif_tx_batch_flush();
        self.tx_batch = batch;
    }

    if (batch && self.tx_batch_remaining) {
        count = self.tx_batch_remaining;
        if (count > UPF_MEMIF_TX_CHUNK)
            count = UPF_MEMIF_TX_CHUNK;
    }

    return preallocate_tx_buffers(count);
}

void upf_n3_memif_tx_batch_begin(uint16_t expected)
{
    ogs_assert(self.tx_count == 0);
    self.tx_batch = true;
    self.tx_batch_remaining = expected;

    if (expected > UPF_MEMIF_MAX_BURST)
        expected = UPF_MEMIF_MAX_BURST;
    if (expected > UPF_MEMIF_TX_CHUNK)
        expected = UPF_MEMIF_TX_CHUNK;
    if (!self.tx_allocated)
        preallocate_tx_buffers(expected);
}

void upf_n3_memif_tx_batch_flush(void)
{
    struct ip *ip_h;
    struct udphdr *udp_h;
    uint16_t sent = 0;
    uint16_t udp_len;
    uint16_t total_len;
    uint16_t checksum;
    uint16_t i;
    uint16_t retained;
    int rv;

    self.tx_batch = false;

    if (!self.tx_count)
        return;

    if (!self.connected) {
        log_tx_drop("disconnected", self.tx_count);
        goto cleanup;
    }

    for (i = 0; i < self.tx_count; i++) {
        ogs_pkbuf_t *gtpu = self.tx_packets[i].pkbuf;
        ogs_sockaddr_t *to = &self.tx_packets[i].to;

        ogs_assert(self.tx_packets[i].ready);
        udp_len = sizeof(*udp_h) + gtpu->len;
        total_len = sizeof(*ip_h) + udp_len;
        memset(self.tx_buffers[i].data, 0,
                sizeof(*ip_h) + sizeof(*udp_h));
        ip_h = self.tx_buffers[i].data;
        udp_h = (struct udphdr *)(
                (uint8_t *)self.tx_buffers[i].data + sizeof(*ip_h));
        ogs_assert(gtpu->data ==
                (uint8_t *)udp_h + sizeof(*udp_h));

        ip_h->ip_v = 4;
        ip_h->ip_hl = sizeof(*ip_h) >> 2;
        ip_h->ip_len = htobe16(total_len);
        ip_h->ip_id = htobe16(++self.ip_id);
        ip_h->ip_off = htobe16(IP_DF);
        ip_h->ip_ttl = 64;
        ip_h->ip_p = IPPROTO_UDP;
        ip_h->ip_src = self.local_addr;
        ip_h->ip_dst = to->sin.sin_addr;
        ip_h->ip_sum = htobe16(ipv4_checksum(ip_h, sizeof(*ip_h)));

        udp_h->uh_sport = htobe16(UPF_GTPU_PORT);
        udp_h->uh_dport = to->ogs_sin_port ?
            to->ogs_sin_port : htobe16(UPF_GTPU_PORT);
        udp_h->uh_ulen = htobe16(udp_len);
        udp_h->uh_sum = 0;
        if (upf_self()->n3.udp_checksum) {
            checksum = udp_checksum(ip_h, udp_h, udp_len);
            udp_h->uh_sum = htobe16(checksum ? checksum : 0xffffU);
        }
        self.tx_buffers[i].len = total_len;
    }

    __atomic_fetch_add(&self.tx_burst_calls, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(
            &self.tx_burst_requested, self.tx_count, __ATOMIC_RELAXED);
    rv = memif_tx_burst(self.connection, upf_dataplane_worker_id(),
            self.tx_buffers, self.tx_count, &sent);
    __atomic_fetch_add(&self.tx_burst_sent, sent, __ATOMIC_RELAXED);
    if (sent != self.tx_count)
        log_tx_drop(memif_strerror(rv), self.tx_count - sent);
    else if (rv != MEMIF_ERR_SUCCESS)
        ogs_warn("N3 memif TX notification failed: %s",
                memif_strerror(rv));

cleanup:
    for (i = 0; i < self.tx_count; i++)
        ogs_pkbuf_free(self.tx_packets[i].pkbuf);
    retained = self.tx_allocated - self.tx_count;
    if (retained) {
        /*
         * Keep the unused contiguous tail for the next burst. Rewinding a
         * busy slave ring is conditional in libmemif and can desynchronize
         * next_buf from ring->head.
         */
        memmove(self.tx_buffers, &self.tx_buffers[self.tx_count],
                retained * sizeof(self.tx_buffers[0]));
    }
    self.tx_count = 0;
    self.tx_allocated = retained;
}

ogs_pkbuf_t *upf_n3_memif_prepare_gtpu(
        const void *payload, uint16_t payload_len, uint8_t gtpu_headroom)
{
    upf_n3_tx_packet_t *packet;
    memif_buffer_t *buffer;
    uint16_t total_len;
    uint32_t capacity;

    ogs_assert(payload);
    ogs_assert(payload_len);

    if (!self.connected || !self.tx_batch ||
        gtpu_headroom < OGS_GTPV1U_HEADER_LEN ||
        gtpu_headroom > OGS_TUN_MAX_HEADROOM)
        return NULL;

    total_len = sizeof(struct ip) + sizeof(struct udphdr) +
        gtpu_headroom + payload_len;
    if (total_len > upf_self()->n3.buffer_size)
        return NULL;

    if (!ensure_tx_buffer())
        return NULL;

    packet = &self.tx_packets[self.tx_count];
    buffer = &self.tx_buffers[self.tx_count];
    memset(packet, 0, sizeof(*packet));

    capacity = buffer->len;
    ogs_assert(capacity >= total_len);
    ogs_pkbuf_init_external(&packet->direct_pkbuf,
            (uint8_t *)buffer->data +
                sizeof(struct ip) + sizeof(struct udphdr),
            capacity - sizeof(struct ip) - sizeof(struct udphdr),
            release_direct_pkbuf);
    ogs_pkbuf_reserve(&packet->direct_pkbuf, gtpu_headroom);
    ogs_pkbuf_put_data(&packet->direct_pkbuf, payload, payload_len);

    packet->pkbuf = &packet->direct_pkbuf;
    self.tx_count++;
    if (self.tx_batch_remaining)
        self.tx_batch_remaining--;
    return packet->pkbuf;
}

int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to)
{
    bool standalone = !self.tx_batch;
    uint16_t total_len;

    ogs_assert(gtpu);
    ogs_assert(to);

    __atomic_fetch_add(&self.tx_send_requests, 1, __ATOMIC_RELAXED);
    if (!self.connected || to->ogs_sa_family != AF_INET) {
        log_tx_drop("disconnected or non-IPv4 peer", 1);
        return OGS_ERROR;
    }
    if (gtpu->len > UINT16_MAX - sizeof(struct ip) - sizeof(struct udphdr)) {
        log_tx_drop("packet length overflow", 1);
        return OGS_ERROR;
    }

    total_len = sizeof(struct ip) + sizeof(struct udphdr) + gtpu->len;
    if (total_len > upf_self()->n3.buffer_size) {
        ogs_warn("[DROP] N3 packet length %u exceeds memif buffer %u",
                total_len, upf_self()->n3.buffer_size);
        log_tx_drop("packet too large", 1);
        return OGS_ERROR;
    }

    if (self.tx_count &&
        self.tx_packets[self.tx_count - 1].pkbuf == gtpu) {
        upf_n3_tx_packet_t *packet = &self.tx_packets[self.tx_count - 1];

        ogs_assert(!packet->ready);
        ogs_pkbuf_ref(gtpu);
        memcpy(&packet->to, to, sizeof(*to));
        packet->ready = true;
        return OGS_OK;
    }

    if (!ensure_tx_buffer()) {
        log_tx_drop("buffer allocation failed", 1);
        return OGS_ERROR;
    }

    {
        upf_n3_tx_packet_t *packet = &self.tx_packets[self.tx_count];
        memif_buffer_t *buffer = &self.tx_buffers[self.tx_count];
        uint32_t capacity = buffer->len;

        memset(packet, 0, sizeof(*packet));
        ogs_pkbuf_init_external(&packet->direct_pkbuf,
                (uint8_t *)buffer->data +
                    sizeof(struct ip) + sizeof(struct udphdr),
                capacity - sizeof(struct ip) - sizeof(struct udphdr),
                release_direct_pkbuf);
        ogs_pkbuf_put_data(&packet->direct_pkbuf, gtpu->data, gtpu->len);
        packet->pkbuf = &packet->direct_pkbuf;
        memcpy(&packet->to, to, sizeof(*to));
        packet->ready = true;
        self.tx_count++;
        if (self.tx_batch_remaining)
            self.tx_batch_remaining--;
    }

    if (standalone)
        upf_n3_memif_tx_batch_flush();
    return OGS_OK;
}

#else /* HAVE_LIBMEMIF */

int upf_n3_memif_open(void)
{
    ogs_error("N3 memif backend requested, but Open5GS lacks libmemif");
    return OGS_ERROR;
}

void upf_n3_memif_close(void)
{
}

bool upf_n3_memif_has_pending(void)
{
    return false;
}

int upf_n3_memif_service(
        uint32_t packet_budget, uint32_t time_budget_us)
{
    (void)packet_budget;
    (void)time_budget_us;
    return OGS_OK;
}

void upf_n3_memif_log_stats(void)
{
}

void upf_n3_memif_tx_batch_begin(uint16_t expected)
{
}

void upf_n3_memif_tx_batch_flush(void)
{
}

ogs_pkbuf_t *upf_n3_memif_prepare_gtpu(
        const void *payload, uint16_t payload_len, uint8_t gtpu_headroom)
{
    return NULL;
}

int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to)
{
    return OGS_ERROR;
}

#endif /* HAVE_LIBMEMIF */
