/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef UPF_RATE_STATS_H
#define UPF_RATE_STATS_H

#include "context.h"

#ifdef __cplusplus
extern "C" {
#endif

int upf_rate_stats_open(void);
void upf_rate_stats_close(void);

void upf_rate_stats_set_user(
        upf_sess_t *sess, const ogs_pfcp_tlv_user_id_t *user_id);
void upf_rate_stats_sync_rules(upf_sess_t *sess);
upf_rate_slot_t *upf_rate_stats_slot(
        upf_sess_t *sess, const ogs_pfcp_pdr_t *pdr);

void upf_rate_stats_batch_begin(void);
void upf_rate_stats_record(upf_rate_slot_t *slot, uint32_t octets);
void upf_rate_stats_batch_end(void);

static inline void upf_rate_stats_tag(
        ogs_pkbuf_t *pkbuf, upf_rate_slot_t *slot, uint32_t octets)
{
    if (!upf_self()->rate_stats.enabled)
        return;
    pkbuf->param[0] = octets;
    pkbuf->param[1] = (uintptr_t)slot;
}

static inline upf_rate_slot_t *upf_rate_stats_tag_slot(
        const ogs_pkbuf_t *pkbuf)
{
    return (upf_rate_slot_t *)(uintptr_t)pkbuf->param[1];
}

static inline uint32_t upf_rate_stats_tag_octets(const ogs_pkbuf_t *pkbuf)
{
    return (uint32_t)pkbuf->param[0];
}

#ifdef __cplusplus
}
#endif

#endif /* UPF_RATE_STATS_H */
