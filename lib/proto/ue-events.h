/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef OGS_UE_EVENTS_H
#define OGS_UE_EVENTS_H

#include "core/ogs-core.h"

/* Value snapshots only. No packet, UE, session or ASN.1 pointers survive a hook. */
typedef struct {
    uint64_t seq, trace_id, parent_seq, flow_id;
    int64_t timestamp, monotonic;
    uint64_t amf_ngap_id, ran_ngap_id, nci, upf_seid;
    uint32_t gnb_id, tac;
    uint8_t gnb_bits, psi, pti, qfi;
    uint8_t psis[16], psi_count;
    uint16_t fiveqi;
    bool action, has_gnb, has_location, has_ngap;
    char supi[64], protocol[12], direction[24], name[64], result[24];
    char cause[96], info[256], plmn[8], cell_plmn[8], peer[80], dnn[101];
} ogs_ue_event_t;

typedef const char *(*ogs_query_arg_t)(void *request, const char *name);
/* The returned JSON is malloc-owned, including on errors. */
char *ogs_ue_events_query(ogs_query_arg_t arg, void *request, int *status);
bool ogs_ue_events_init(const char *nf, size_t capacity, size_t per_ue);
void ogs_ue_events_open(const char *nf);
void ogs_ue_events_final(void);
bool ogs_ue_events_enabled(void);
uint64_t ogs_ue_events_new_trace(void);
uint64_t ogs_ue_events_record(const ogs_ue_event_t *event);
void ogs_ue_events_bind(uint64_t trace_id, const char *supi);

#endif
