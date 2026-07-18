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

#if HAVE_LIBMEMIF
#include <libmemif.h>

#define UPF_MEMIF_MAX_BURST 256

static struct {
    memif_socket_handle_t socket;
    memif_conn_handle_t connection;
    ogs_pkbuf_t *tx_packets[UPF_MEMIF_MAX_BURST];
    uint16_t tx_count;
    uint64_t tx_drops;
    ogs_time_t last_tx_drop_log;
    bool tx_batch;
    bool connected;
} self;

static void log_tx_drop(const char *reason)
{
    ogs_time_t now = ogs_get_monotonic_time();

    self.tx_drops++;
    if (!self.last_tx_drop_log ||
        now - self.last_tx_drop_log >= ogs_time_from_sec(1)) {
        ogs_warn("[DROP] N6 memif TX failed [%s, total:%llu]",
                reason, (unsigned long long)self.tx_drops);
        self.last_tx_drop_log = now;
    }
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

        upf_n3_memif_tx_batch_begin();
        for (i = 0; i < count; i++) {
            if (buffers[i].flags & MEMIF_BUFFER_FLAG_NEXT) {
                ogs_error("[DROP] Chained N6 memif buffers are not supported");
                continue;
            }
            if (upf_gtp_handle_n6_data(buffers[i].data, buffers[i].len) !=
                    OGS_OK)
                ogs_error("[DROP] Invalid packet received from N6 memif");
        }
        upf_n3_memif_tx_batch_flush();

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

    memset(&self, 0, sizeof(self));

    memset(&socket_args, 0, sizeof(socket_args));
    ogs_cpystrn(socket_args.path, upf_self()->n6.socket_path,
            sizeof(socket_args.path));
    ogs_cpystrn(socket_args.app_name, "open5gs-upf",
            sizeof(socket_args.app_name));
    socket_args.connection_request_timer.it_value.tv_sec = 1;
    socket_args.connection_request_timer.it_interval.tv_sec = 1;

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
    upf_n6_memif_tx_batch_flush();
    self.connected = false;
    if (self.connection)
        memif_delete(&self.connection);
    if (self.socket)
        memif_delete_socket(&self.socket);

}

int upf_n6_memif_poll(void)
{
    int rv;

    if (!self.socket)
        return OGS_DONE;

    rv = memif_poll_event(self.socket, 0);
    if (rv == MEMIF_ERR_SUCCESS || rv == MEMIF_ERR_AGAIN)
        return OGS_OK;
    if (rv == MEMIF_ERR_POLL_CANCEL)
        return OGS_DONE;

    ogs_error("N6 memif_poll_event() failed: %s", memif_strerror(rv));
    return OGS_ERROR;
}

void upf_n6_memif_cancel_poll(void)
{
    if (self.socket)
        memif_cancel_poll_event(self.socket);
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
        log_tx_drop("disconnected");
        goto cleanup;
    }

    for (i = 0; i < self.tx_count; i++) {
        if (self.tx_packets[i]->len > max_len)
            max_len = self.tx_packets[i]->len;
    }

    rv = memif_buffer_alloc(self.connection, 0, buffers, self.tx_count,
            &allocated, max_len);
    if ((rv != MEMIF_ERR_SUCCESS && rv != MEMIF_ERR_NOBUF_RING &&
            rv != MEMIF_ERR_NOBUF) || !allocated) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_buffer_alloc() failed: %s", memif_strerror(rv));
        log_tx_drop(memif_strerror(rv));
        goto cleanup;
    }

    for (i = 0; i < allocated; i++) {
        memcpy(buffers[i].data, self.tx_packets[i]->data,
                self.tx_packets[i]->len);
        buffers[i].len = self.tx_packets[i]->len;
    }

    rv = memif_tx_burst(self.connection, 0, buffers, allocated, &sent);
    if (rv != MEMIF_ERR_SUCCESS || sent != allocated) {
        if (rv != MEMIF_ERR_NOBUF_RING && rv != MEMIF_ERR_NOBUF)
            ogs_error("memif_tx_burst() failed: %s", memif_strerror(rv));
        log_tx_drop(memif_strerror(rv));
    }
    if (allocated != self.tx_count)
        log_tx_drop("partial batch allocation");

cleanup:
    for (i = 0; i < self.tx_count; i++)
        ogs_pkbuf_free(self.tx_packets[i]);
    self.tx_count = 0;
}

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf)
{
    bool standalone = !self.tx_batch;

    ogs_assert(pkbuf);

    if (pkbuf->len > upf_self()->n6.buffer_size || pkbuf->len > UINT16_MAX) {
        log_tx_drop("packet too large");
        return OGS_ERROR;
    }

    if (self.tx_count == UPF_MEMIF_MAX_BURST) {
        upf_n6_memif_tx_batch_flush();
        self.tx_batch = true;
    }

    ogs_pkbuf_ref((ogs_pkbuf_t *)pkbuf);
    self.tx_packets[self.tx_count++] = (ogs_pkbuf_t *)pkbuf;

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

int upf_n6_memif_poll(void)
{
    return OGS_DONE;
}

void upf_n6_memif_cancel_poll(void)
{
}

void upf_n6_memif_tx_batch_begin(void)
{
}

void upf_n6_memif_tx_batch_flush(void)
{
}

int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf)
{
    return OGS_ERROR;
}

#endif /* HAVE_LIBMEMIF */
