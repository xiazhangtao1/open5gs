/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "context.h"
#include "gtp-path.h"
#include "n3-memif.h"

#include <netinet/ip.h>
#include <netinet/udp.h>

#if HAVE_LIBMEMIF
#include <libmemif.h>

#define UPF_MEMIF_MAX_CONTROL_FDS 64
#define UPF_MEMIF_MAX_BURST 256
#define UPF_GTPU_PORT 2152

typedef struct upf_n3_memif_fd_s {
    int fd;
    void *private_ctx;
    ogs_poll_t *read_poll;
    ogs_poll_t *write_poll;
} upf_n3_memif_fd_t;

static struct {
    memif_socket_handle_t socket;
    memif_conn_handle_t connection;
    upf_n3_memif_fd_t fds[UPF_MEMIF_MAX_CONTROL_FDS];
    struct in_addr local_addr;
    uint16_t ip_id;
    bool connected;
} self;

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

static upf_n3_memif_fd_t *find_fd(int fd)
{
    int i;

    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++) {
        if (self.fds[i].fd == fd)
            return &self.fds[i];
    }
    return NULL;
}

static upf_n3_memif_fd_t *alloc_fd(int fd)
{
    int i;

    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++) {
        if (self.fds[i].fd == -1) {
            self.fds[i].fd = fd;
            return &self.fds[i];
        }
    }
    return NULL;
}

static void remove_polls(upf_n3_memif_fd_t *entry)
{
    if (entry->read_poll) {
        ogs_pollset_remove(entry->read_poll);
        entry->read_poll = NULL;
    }
    if (entry->write_poll) {
        ogs_pollset_remove(entry->write_poll);
        entry->write_poll = NULL;
    }
}

static void control_fd_cb(short when, ogs_socket_t fd, void *data)
{
    upf_n3_memif_fd_t *entry = data;
    memif_fd_event_type_t events = 0;
    int rv;

    ogs_assert(entry);
    ogs_assert(fd == entry->fd);

    if (when & OGS_POLLIN)
        events |= MEMIF_FD_EVENT_READ;
    if (when & OGS_POLLOUT)
        events |= MEMIF_FD_EVENT_WRITE;

    rv = memif_control_fd_handler(entry->private_ctx, events);
    if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_AGAIN)
        ogs_error("N3 memif_control_fd_handler(%d) failed: %s",
                fd, memif_strerror(rv));
}

static int control_fd_update(memif_fd_event_t event, void *private_ctx)
{
    upf_n3_memif_fd_t *entry = find_fd(event.fd);

    if (event.type & MEMIF_FD_EVENT_DEL) {
        if (entry) {
            remove_polls(entry);
            entry->fd = -1;
            entry->private_ctx = NULL;
        }
        return MEMIF_ERR_SUCCESS;
    }

    if (!entry)
        entry = alloc_fd(event.fd);
    if (!entry) {
        ogs_error("Too many N3 memif control file descriptors");
        return MEMIF_ERR_NOMEM;
    }

    entry->private_ctx = event.private_ctx;
    if (event.type & MEMIF_FD_EVENT_MOD)
        remove_polls(entry);

    if ((event.type & MEMIF_FD_EVENT_READ) && !entry->read_poll) {
        entry->read_poll = ogs_pollset_add(ogs_app()->pollset,
                OGS_POLLIN, event.fd, control_fd_cb, entry);
        if (!entry->read_poll)
            return MEMIF_ERR_CB_FDUPDATE;
    }
    if ((event.type & MEMIF_FD_EVENT_WRITE) && !entry->write_poll) {
        entry->write_poll = ogs_pollset_add(ogs_app()->pollset,
                OGS_POLLOUT, event.fd, control_fd_cb, entry);
        if (!entry->write_poll)
            return MEMIF_ERR_CB_FDUPDATE;
    }
    return MEMIF_ERR_SUCCESS;
}

static int handle_raw_ipv4(const void *data, uint16_t len)
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

    return upf_gtp_handle_n3_data(
            (const uint8_t *)udp_h + sizeof(*udp_h),
            udp_len - sizeof(*udp_h), &from);
}

static int on_connect(memif_conn_handle_t connection, void *private_ctx)
{
    int rv = memif_refill_queue(connection, 0, UINT16_MAX, 0);

    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("N3 memif_refill_queue() failed: %s", memif_strerror(rv));
        return rv;
    }
    self.connected = true;
    ogs_info("N3 memif connected [socket:%s id:%u]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_disconnect(memif_conn_handle_t connection, void *private_ctx)
{
    self.connected = false;
    ogs_warn("N3 memif disconnected [socket:%s id:%u]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_interrupt(
        memif_conn_handle_t connection, void *private_ctx, uint16_t qid)
{
    memif_buffer_t buffers[UPF_MEMIF_MAX_BURST];
    uint16_t count = 0;
    uint16_t limit = upf_self()->n3.burst_size;
    int rv;
    int i;

    do {
        rv = memif_rx_burst(connection, qid, buffers, limit, &count);
        if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF) {
            ogs_error("N3 memif_rx_burst(qid:%u) failed: %s",
                    qid, memif_strerror(rv));
            return rv;
        }

        for (i = 0; i < count; i++) {
            if (buffers[i].flags & MEMIF_BUFFER_FLAG_NEXT) {
                ogs_error("[DROP] Chained N3 memif buffers are unsupported");
                continue;
            }
            if (handle_raw_ipv4(buffers[i].data, buffers[i].len) != OGS_OK)
                ogs_debug("[DROP] Invalid packet received from N3 memif");
        }

        rv = memif_refill_queue(connection, qid, count, 0);
        if (rv != MEMIF_ERR_SUCCESS) {
            ogs_error("N3 memif_refill_queue(qid:%u count:%u) failed: %s",
                    qid, count, memif_strerror(rv));
            return rv;
        }
    } while (count == limit);

    return MEMIF_ERR_SUCCESS;
}

int upf_n3_memif_open(void)
{
    memif_socket_args_t socket_args;
    memif_conn_args_t connection_args;
    ogs_sockaddr_t local;
    int rv;
    int i;

    memset(&self, 0, sizeof(self));
    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++)
        self.fds[i].fd = -1;

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
    socket_args.on_control_fd_update = control_fd_update;

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
    connection_args.num_s2m_rings = 1;
    connection_args.num_m2s_rings = 1;
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
    ogs_info("N3 memif initialized [socket:%s id:%u address:%s ring:%u]",
            upf_self()->n3.socket_path, upf_self()->n3.interface_id,
            upf_self()->n3.local_address,
            1U << upf_self()->n3.log2_ring_size);
    return OGS_OK;
}

void upf_n3_memif_close(void)
{
    int i;

    ogs_gtp_set_user_plane_send_cb(NULL);
    self.connected = false;
    if (self.connection)
        memif_delete(&self.connection);
    if (self.socket)
        memif_delete_socket(&self.socket);

    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++) {
        remove_polls(&self.fds[i]);
        self.fds[i].fd = -1;
        self.fds[i].private_ctx = NULL;
    }
}

int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to)
{
    memif_buffer_t buffer;
    struct ip *ip_h;
    struct udphdr *udp_h;
    uint16_t allocated = 0;
    uint16_t sent = 0;
    uint16_t udp_len;
    uint16_t total_len;
    uint16_t checksum;
    int rv;

    ogs_assert(gtpu);
    ogs_assert(to);

    if (!self.connected || to->ogs_sa_family != AF_INET)
        return OGS_ERROR;
    if (gtpu->len > UINT16_MAX - sizeof(*ip_h) - sizeof(*udp_h))
        return OGS_ERROR;

    udp_len = sizeof(*udp_h) + gtpu->len;
    total_len = sizeof(*ip_h) + udp_len;
    if (total_len > upf_self()->n3.buffer_size) {
        ogs_warn("[DROP] N3 packet length %u exceeds memif buffer %u",
                total_len, upf_self()->n3.buffer_size);
        return OGS_ERROR;
    }

    rv = memif_buffer_alloc(self.connection, 0, &buffer, 1,
            &allocated, total_len);
    if (rv != MEMIF_ERR_SUCCESS || allocated != 1)
        return OGS_ERROR;

    memset(buffer.data, 0, sizeof(*ip_h) + sizeof(*udp_h));
    ip_h = buffer.data;
    udp_h = (struct udphdr *)((uint8_t *)buffer.data + sizeof(*ip_h));
    memcpy((uint8_t *)udp_h + sizeof(*udp_h), gtpu->data, gtpu->len);

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
    checksum = udp_checksum(ip_h, udp_h, udp_len);
    udp_h->uh_sum = htobe16(checksum ? checksum : 0xffffU);
    buffer.len = total_len;

    rv = memif_tx_burst(self.connection, 0, &buffer, 1, &sent);
    if (rv != MEMIF_ERR_SUCCESS || sent != 1)
        return OGS_ERROR;
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

int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to)
{
    return OGS_ERROR;
}

#endif /* HAVE_LIBMEMIF */
