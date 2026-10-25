/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef SMF_SESSION_CONTROL_H
#define SMF_SESSION_CONTROL_H
#include "context.h"
bool smf_session_control_request(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request);
bool smf_session_control_busy(const smf_sess_t *sess);
bool smf_session_control_connected(const smf_sess_t *sess);
bool smf_session_control_pending(const smf_sess_t *sess);
bool smf_session_control_qos_changed(const smf_sess_t *sess);
bool smf_session_control_ambr_changed(const smf_sess_t *sess);
uint64_t smf_session_control_generation(const smf_sess_t *sess);
void smf_session_control_pfcp(smf_sess_t *sess, bool success);
bool smf_session_control_nas(smf_sess_t *sess, uint8_t pti, bool success);
bool smf_session_control_n2(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_pkbuf_t *pkbuf, bool success);
void smf_session_control_event(smf_event_t *event);
void smf_session_control_clear(smf_sess_t *sess);
/* The metrics HTTP server runs on the SMF pollset, alongside state updates. */
cJSON *smf_session_control_snapshot(const smf_sess_t *sess);
/* Stop a pending transaction when its session is being removed or released. */
void smf_session_control_cancel(smf_sess_t *sess);
#endif
