/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UPF_N3_MEMIF_H
#define UPF_N3_MEMIF_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

int upf_n3_memif_open(void);
void upf_n3_memif_close(void);
bool upf_n3_memif_has_pending(void);
int upf_n3_memif_service(uint32_t packet_budget, uint32_t time_budget_us);
void upf_n3_memif_log_stats(void);
void upf_n3_memif_complete(
        uint16_t qid, uint64_t sequence, uint32_t generation);
void upf_n3_memif_drain_completions(void);
void upf_n3_memif_tx_batch_begin(uint16_t expected);
void upf_n3_memif_tx_batch_flush(void);
ogs_pkbuf_t *upf_n3_memif_prepare_gtpu(
        const void *payload, uint16_t payload_len, uint8_t gtpu_headroom);
int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to);

#ifdef __cplusplus
}
#endif

#endif /* UPF_N3_MEMIF_H */
