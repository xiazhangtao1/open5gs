/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UPF_DATAPLANE_H
#define UPF_DATAPLANE_H

#include "ogs-core.h"

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

#ifdef __cplusplus
}
#endif

#endif /* UPF_DATAPLANE_H */
