/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef OGS_SBI_SESSION_CONTROL_H
#define OGS_SBI_SESSION_CONTROL_H

#include "ogs-sbi.h"

typedef struct {
    ogs_qos_t qos;
    ogs_bitrate_t ambr;
} ogs_sbi_session_config_t;

enum {
    OGS_SESSION_CONTROL_NONE, OGS_SESSION_CONTROL_PENDING,
    OGS_SESSION_CONTROL_CONFIRMED, OGS_SESSION_CONTROL_FAILED,
    OGS_SESSION_CONTROL_UNKNOWN
};
typedef struct {
    char instance[OGS_UUID_FORMATTED_LENGTH + 1];
    char transaction[OGS_UUID_FORMATTED_LENGTH + 1];
    char operation_id[81];
    int state, pfcp, nas, ngap;
    char detail[160];
    ogs_time_t updated_at;
    ogs_sbi_session_config_t previous, target;
} ogs_sbi_session_operation_t;

bool ogs_sbi_session_config_parse(const cJSON *body,
        ogs_sbi_session_config_t *config);
cJSON *ogs_sbi_session_config_json(const ogs_sbi_session_config_t *config);
bool ogs_sbi_session_qos_equal(const ogs_qos_t *a, const ogs_qos_t *b);
bool ogs_sbi_session_config_equal(const ogs_sbi_session_config_t *a,
        const ogs_sbi_session_config_t *b);
bool ogs_sbi_session_bitrate_parse(const char *text, uint64_t *bps);
bool ogs_sbi_session_ambr_encode(uint64_t bps, uint8_t *unit, uint16_t *value);
bool ogs_sbi_session_control_id(const char *text, bool uuid);
const char *ogs_sbi_session_control_state(int state);
cJSON *ogs_sbi_session_operation_json(const ogs_sbi_session_operation_t *op);
/* Validate a final result, including every required stage. Identity is checked
 * against the live transaction by the PCF caller before accepting it. */
bool ogs_sbi_session_operation_result(const cJSON *body,
        ogs_sbi_session_operation_t *op);

#endif
