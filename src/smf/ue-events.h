/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef SMF_UE_EVENTS_H
#define SMF_UE_EVENTS_H
#include "context.h"
#include "proto/ue-events.h"
void smf_ue_event(smf_sess_t *sess, const char *protocol, const char *direction,
        const char *name, const char *result, const char *cause, const char *info, bool action);
void smf_ue_event_qos(smf_sess_t *sess, const char *result, unsigned qfi, unsigned fiveqi, const char *info);
void smf_ue_event_nas(smf_sess_t *sess, ogs_nas_5gs_message_t *message, bool received);
void smf_ue_event_pfcp(smf_sess_t *sess, uint8_t type, const char *result, uint8_t cause);
void smf_ue_event_sbi(smf_sess_t *sess, ogs_sbi_message_t *message, bool received);
void smf_ue_event_sbi_request(smf_sess_t *sess, ogs_sbi_request_t *request, const char *result);
void smf_ue_event_establishment_end(smf_sess_t *sess, const char *result, const char *info);
void smf_ue_event_establishment_n4(smf_sess_t *sess, ogs_pool_id_t xact_id,
        const char *result, const char *info);
#endif
