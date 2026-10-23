/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef AMF_UE_EVENTS_H
#define AMF_UE_EVENTS_H
#include "context.h"
#include "proto/ue-events.h"
uint64_t amf_ue_event(ran_ue_t *ran, amf_ue_t *ue, const char *protocol,
        const char *direction, const char *name, const char *result,
        const char *cause, const char *info, bool action);
uint64_t amf_ue_event_ngap(amf_gnb_t *gnb, ogs_ngap_message_t *message,
        const char *direction, const char *result);
uint64_t amf_ue_event_nas(ran_ue_t *ran, amf_ue_t *ue,
        ogs_nas_5gs_message_t *message, bool received);
ogs_pkbuf_t *amf_ue_event_nas_encode(amf_ue_t *ue, ogs_nas_5gs_message_t *message);
void amf_ue_event_session(amf_ue_t *ue, ran_ue_t *ran, amf_sess_t *sess,
        ogs_sbi_message_t *message, const char *name);
#endif
