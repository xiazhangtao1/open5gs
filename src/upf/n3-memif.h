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
int upf_n3_memif_send_gtpu(
        ogs_pkbuf_t *gtpu, const ogs_sockaddr_t *to);

#ifdef __cplusplus
}
#endif

#endif /* UPF_N3_MEMIF_H */
