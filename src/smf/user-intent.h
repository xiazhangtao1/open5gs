/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef SMF_USER_INTENT_H
#define SMF_USER_INTENT_H
#include "context.h"

typedef struct {
    uint8_t comm_status, ai_model, gpu_level;
} smf_intent_value_t;
typedef enum {
    SMF_INTENT_NONE, SMF_INTENT_CREATE, SMF_INTENT_MODIFY, SMF_INTENT_DELETE
} smf_intent_operation_t;
typedef struct {
    smf_intent_operation_t operation;
    uint8_t five_qi, priority;
} smf_intent_plan_t;

/* OGS_OK: found; OGS_DONE: absent; OGS_ERROR: invalid. */
int smf_intent_parse(const uint8_t *data, size_t length, uint16_t id,
        const ogs_plmn_id_t *plmn, smf_intent_value_t *value);
smf_intent_plan_t smf_intent_plan(const smf_intent_value_t *value,
        bool exists, uint8_t five_qi, uint8_t priority);
cJSON *smf_intent_notification(const smf_intent_value_t *value,
        const char *ue_ip, const char *supi, bool success, uint8_t five_qi);
int smf_user_intent_config_validate(void);
int smf_intent_epco_response(uint8_t *result, size_t capacity,
        const uint8_t *data, size_t data_length);
/* true means this request was handled (including a controlled rejection). */
bool smf_user_intent_request(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_nas_5gs_pdu_session_modification_request_t *request);
bool smf_user_intent_busy(smf_sess_t *sess);
bool smf_user_intent_binding(smf_sess_t *sess);
void smf_user_intent_resume(smf_sess_t *sess);
uint64_t smf_user_intent_generation(smf_sess_t *sess);
void smf_user_intent_event(smf_event_t *e);
void smf_user_intent_pfcp(smf_sess_t *sess, uint64_t flags, bool success);
void smf_user_intent_rejected(smf_sess_t *sess, uint8_t pti);
void smf_user_intent_complete(smf_sess_t *sess, uint8_t pti);
bool smf_user_intent_n2(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_pkbuf_t *pkbuf);
void smf_user_intent_clear(smf_sess_t *sess);
#endif
