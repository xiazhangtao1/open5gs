/*
 * Copyright (C) 2026 by Open5GS contributors
 *
 * This file is part of Open5GS.
 */

#ifndef UPF_N6_MEMIF_H
#define UPF_N6_MEMIF_H

#include "ogs-core.h"

#ifdef __cplusplus
extern "C" {
#endif

int upf_n6_memif_open(void);
void upf_n6_memif_close(void);
bool upf_n6_memif_has_pending(void);
int upf_n6_memif_service(uint32_t packet_budget, uint32_t time_budget_us);
void upf_n6_memif_log_stats(void);
void upf_n6_memif_complete(
        uint16_t qid, uint64_t sequence, uint32_t generation);
void upf_n6_memif_drain_completions(void);
void upf_n6_memif_tx_batch_begin(void);
void upf_n6_memif_tx_batch_flush(void);
int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf);

#ifdef __cplusplus
}
#endif

#endif /* UPF_N6_MEMIF_H */
