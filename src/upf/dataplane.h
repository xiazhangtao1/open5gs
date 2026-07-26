/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UPF_DATAPLANE_H
#define UPF_DATAPLANE_H

#include "ogs-core.h"

#define UPF_DATAPLANE_MAX_BURST 256

#if HAVE_LIBMEMIF
#include <libmemif.h>
#endif

typedef struct {
    const void *data;
    size_t len;
    ogs_sockaddr_t from;
} upf_dataplane_packet_t;

#ifdef __cplusplus
extern "C" {
#endif

void upf_dataplane_init(void);
void upf_dataplane_final(void);
int upf_dataplane_start(void);
void upf_dataplane_stop(void);

void upf_dataplane_lock(void);
void upf_dataplane_unlock(void);
void upf_dataplane_read_lock(void);
void upf_dataplane_read_unlock(void);
uint8_t upf_dataplane_worker_id(void);
uint8_t upf_dataplane_assign_session(uint64_t seid);
void upf_dataplane_release_session(uint8_t owner);
void upf_dataplane_report_lock(void);
void upf_dataplane_report_unlock(void);
int upf_dataplane_submit_n3(
        const void *data, size_t len, const ogs_sockaddr_t *from);
int upf_dataplane_submit_n6(const void *data, size_t len);
int upf_dataplane_submit_n3_batch(
        const upf_dataplane_packet_t packets[], uint16_t count,
        uint16_t *submitted);
int upf_dataplane_submit_n6_batch(
        const upf_dataplane_packet_t packets[], uint16_t count,
        uint16_t *submitted);

#if HAVE_LIBMEMIF
int upf_dataplane_memif_fd_update(
        memif_fd_event_t fde, void *private_ctx);
#endif
void upf_dataplane_wake(void);

#ifdef __cplusplus
}
#endif

#endif /* UPF_DATAPLANE_H */
