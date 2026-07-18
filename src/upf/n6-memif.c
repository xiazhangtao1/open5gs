/*
 * Copyright (C) 2026 by Open5GS contributors
 *
 * This file is part of Open5GS.
 */

#include "context.h"
#include "gtp-path.h"
#include "n6-memif.h"

#if HAVE_LIBMEMIF
#include <libmemif.h>

#define UPF_MEMIF_MAX_CONTROL_FDS 64
#define UPF_MEMIF_MAX_BURST 256

typedef struct upf_memif_fd_s {
    int fd;
    void *private_ctx;
    ogs_poll_t *read_poll;
    ogs_poll_t *write_poll;
} upf_memif_fd_t;

static struct {
    memif_socket_handle_t socket;
    memif_conn_handle_t connection;
    upf_memif_fd_t fds[UPF_MEMIF_MAX_CONTROL_FDS];
    bool connected;
} self;

static upf_memif_fd_t *find_fd(int fd)
{
    int i;

    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++) {
        if (self.fds[i].fd == fd)
            return &self.fds[i];
    }
    return NULL;
}

static upf_memif_fd_t *alloc_fd(int fd)
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

static void remove_polls(upf_memif_fd_t *entry)
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
    upf_memif_fd_t *entry = data;
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
        ogs_error("memif_control_fd_handler(%d) failed: %s",
                fd, memif_strerror(rv));
}

static int control_fd_update(memif_fd_event_t event, void *private_ctx)
{
    upf_memif_fd_t *entry = find_fd(event.fd);

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
        ogs_error("Too many memif control file descriptors");
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

static int on_connect(memif_conn_handle_t connection, void *private_ctx)
{
    int rv;

    rv = memif_refill_queue(connection, 0, UINT16_MAX, 0);
    if (rv != MEMIF_ERR_SUCCESS) {
        ogs_error("memif_refill_queue() failed: %s", memif_strerror(rv));
        return rv;
    }

    self.connected = true;
    ogs_info("N6 memif connected [socket:%s id:%u]",
            upf_self()->n6.socket_path, upf_self()->n6.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_disconnect(memif_conn_handle_t connection, void *private_ctx)
{
    self.connected = false;
    ogs_warn("N6 memif disconnected [socket:%s id:%u]",
            upf_self()->n6.socket_path, upf_self()->n6.interface_id);
    return MEMIF_ERR_SUCCESS;
}

static int on_interrupt(
        memif_conn_handle_t connection, void *private_ctx, uint16_t qid)
{
    memif_buffer_t buffers[UPF_MEMIF_MAX_BURST];
    uint16_t count = 0;
    uint16_t limit = upf_self()->n6.burst_size;
    int rv;
    int i;

    do {
        rv = memif_rx_burst(connection, qid, buffers, limit, &count);
        if (rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF) {
            ogs_error("memif_rx_burst(qid:%u) failed: %s",
                    qid, memif_strerror(rv));
            return rv;
        }

        for (i = 0; i < count; i++) {
            if (buffers[i].flags & MEMIF_BUFFER_FLAG_NEXT) {
                ogs_error("[DROP] Chained N6 memif buffers are not supported");
                continue;
            }
            if (upf_gtp_handle_n6_data(buffers[i].data, buffers[i].len) !=
                    OGS_OK)
                ogs_error("[DROP] Invalid packet received from N6 memif");
        }

        rv = memif_refill_queue(connection, qid, count, 0);
        if (rv != MEMIF_ERR_SUCCESS) {
            ogs_error("memif_refill_queue(qid:%u count:%u) failed: %s",
                    qid, count, memif_strerror(rv));
            return rv;
        }
    } while (count == limit);

    return MEMIF_ERR_SUCCESS;
}

int upf_n6_memif_open(void)
{
    memif_socket_args_t socket_args;
    memif_conn_args_t connection_args;
    int rv;
    int i;

    memset(&self, 0, sizeof(self));
    for (i = 0; i < UPF_MEMIF_MAX_CONTROL_FDS; i++)
        self.fds[i].fd = -1;

    memset(&socket_args, 0, sizeof(socket_args));
    ogs_cpystrn(socket_args.path, upf_self()->n6.socket_path,
            sizeof(socket_args.path));
    ogs_cpystrn(socket_args.app_name, "open5gs-upf",
            sizeof(socket_args.app_name));
    socket_args.connection_request_timer.it_value.tv_sec = 1;
    socket_args.connection_request_timer.it_interval.tv_sec = 1;
    socket_args.on_control_fd_update = control_fd_update;

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
    connection_args.num_s2m_rings = 1;
    connection_args.num_m2s_rings = 1;
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
    int i;

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

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf)
{
    memif_buffer_t buffer;
    uint16_t allocated = 0;
    uint16_t sent = 0;
    int rv;

    ogs_assert(pkbuf);

    if (!self.connected)
        return OGS_ERROR;
    if (pkbuf->len > upf_self()->n6.buffer_size || pkbuf->len > UINT16_MAX) {
        ogs_error("[DROP] N6 packet length %u exceeds memif buffer %u",
                pkbuf->len, upf_self()->n6.buffer_size);
        return OGS_ERROR;
    }

    rv = memif_buffer_alloc(self.connection, 0, &buffer, 1,
            &allocated, (uint16_t)pkbuf->len);
    if (rv != MEMIF_ERR_SUCCESS || allocated != 1) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_buffer_alloc() failed: %s", memif_strerror(rv));
        return OGS_ERROR;
    }

    memcpy(buffer.data, pkbuf->data, pkbuf->len);
    buffer.len = pkbuf->len;
    rv = memif_tx_burst(self.connection, 0, &buffer, 1, &sent);
    if (rv != MEMIF_ERR_SUCCESS || sent != 1) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_tx_burst() failed: %s", memif_strerror(rv));
        return OGS_ERROR;
    }

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

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf)
{
    return OGS_ERROR;
}

#endif /* HAVE_LIBMEMIF */
