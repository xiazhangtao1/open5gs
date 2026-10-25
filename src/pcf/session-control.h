/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef PCF_SESSION_CONTROL_H
#define PCF_SESSION_CONTROL_H
#include "context.h"
void pcf_session_control_initial(pcf_sess_t *sess, OpenAPI_session_rule_t *rule);
void pcf_session_control_json(pcf_sess_t *sess, cJSON *body);
bool pcf_session_control_busy(pcf_sess_t *sess, bool release);
bool pcf_session_control_update(pcf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request);
bool pcf_session_control_result(pcf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request);
#endif
