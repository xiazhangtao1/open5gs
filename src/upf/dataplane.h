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

#ifdef __cplusplus
}
#endif

#endif /* UPF_DATAPLANE_H */
