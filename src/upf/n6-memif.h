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
int upf_n6_memif_poll(void);
void upf_n6_memif_cancel_poll(void);
void upf_n6_memif_tx_batch_begin(void);
void upf_n6_memif_tx_batch_flush(void);
int upf_n6_memif_send(const ogs_pkbuf_t *pkbuf);

#ifdef __cplusplus
}
#endif

#endif /* UPF_N6_MEMIF_H */
