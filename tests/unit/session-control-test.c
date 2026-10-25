/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "sbi/session-control.h"
#include "ogs-nas-5gs.h"
#include "core/abts.h"
#include "../../src/smf/session-control.h"

static cJSON *configuration(void)
{
    return cJSON_Parse("{\"defaultQos\":{\"5qi\":9,\"priorityLevel\":90,\"arp\":{"
        "\"priorityLevel\":8,\"preemptionCapability\":\"NOT_PREEMPT\","
        "\"preemptionVulnerability\":\"PREEMPTABLE\"}},"
        "\"sessionAmbr\":{\"uplink\":\"10 Mbps\",\"downlink\":\"20 Mbps\"}}");
}

static void bitrate_test(abts_case *tc, void *data)
{
    static const char *invalid[] = {NULL, "", "-1 Mbps", "0 Mbps", "NaN Mbps",
        "1e3 Mbps", "10Mbps", "4.000001 Tbps", "1 bps", "0.0000001 Mbps",
        "18446744073709551616 bps", "1. Mbps", "1 Mbps extra", "65535001 bps"};
    static const uint64_t valid[] = {1000, 65535000, 65536000, 100000000,
        1000000000, 10000000000ULL, 17000000000ULL, 65000000000ULL, 4000000000000ULL};
    uint64_t bps;
    unsigned i;
    uint8_t unit;
    uint16_t value;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        ABTS_INT_EQUAL(tc, false, ogs_sbi_session_bitrate_parse(invalid[i], &bps));
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_bitrate_parse("0.000001 Tbps", &bps));
    ABTS_INT_EQUAL(tc, 1000000, bps);
    for (i = 0; i < sizeof(valid) / sizeof(valid[0]); i++) {
        ogs_nas_bitrate_t wire = {0};
        ABTS_INT_EQUAL(tc, true, ogs_sbi_session_ambr_encode(valid[i], &unit, &value));
        wire.unit = unit; wire.value = value;
        ABTS_TRUE(tc, ogs_nas_bitrate_to_uint64(&wire) == valid[i]);
        ogs_nas_bitrate_from_uint64(&wire, valid[i]);
        ABTS_TRUE(tc, ogs_nas_bitrate_to_uint64(&wire) == valid[i]);
    }
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_ambr_encode(UINT64_MAX, NULL, NULL));
    {
        ogs_nas_bitrate_t wire = {0};
        ogs_nas_bitrate_from_uint64(&wire, 17000000000ULL);
        ABTS_TRUE(tc, ogs_nas_bitrate_to_uint64(&wire) == 17000000000ULL);
        wire.unit = OGS_NAS_BR_UNIT_256P; wire.value = UINT16_MAX;
        ABTS_TRUE(tc, ogs_nas_bitrate_to_uint64(&wire) == 0);
    }
}

static void config_test(abts_case *tc, void *data)
{
    cJSON *body = configuration(), *q = cJSON_GetObjectItemCaseSensitive(body, "defaultQos");
    cJSON *arp = cJSON_GetObjectItemCaseSensitive(q, "arp"), *roundtrip;
    ogs_sbi_session_config_t value, copy;
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_config_parse(body, &value));
    ABTS_INT_EQUAL(tc, 90, value.qos.priority_level);
    ABTS_INT_EQUAL(tc, 8, value.qos.arp.priority_level);
    ABTS_INT_EQUAL(tc, 10000000, value.ambr.uplink);
    roundtrip = ogs_sbi_session_config_json(&value);
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_config_parse(roundtrip, &copy));
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_config_equal(&value, &copy));
    cJSON_Delete(roundtrip);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(q, "5qi"), 1);
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_config_parse(body, &copy));
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(q, "5qi"), 8);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(q, "priorityLevel"), 128);
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_config_parse(body, &copy));
    cJSON_DeleteItemFromObjectCaseSensitive(q, "priorityLevel");
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_config_parse(body, &copy));
    ABTS_INT_EQUAL(tc, 0, copy.qos.priority_level);
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(arp, "priorityLevel"), 16);
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_config_parse(body, &copy));
    cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(arp, "priorityLevel"), 8);
    cJSON_AddNumberToObject(q, "5qi", 9);
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_config_parse(body, &copy));
    cJSON_Delete(body);
    body = configuration(); cJSON_AddStringToObject(body, "supi", "imsi-460110000000100");
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_config_parse(body, &copy));
    cJSON_Delete(body);
}

static void operation_test(abts_case *tc, void *data)
{
    ogs_sbi_session_operation_t op = {0}, parsed = {0};
    cJSON *body, *stages;
    op.state = op.pfcp = op.ngap = OGS_SESSION_CONTROL_CONFIRMED;
    ogs_cpystrn(op.detail, "Required replies received", sizeof(op.detail));
    body = ogs_sbi_session_operation_json(&op);
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_operation_result(body, &parsed));
    stages = cJSON_GetObjectItemCaseSensitive(body, "stages");
    cJSON_ReplaceItemInObjectCaseSensitive(stages, "nas", cJSON_CreateString("pending"));
    ABTS_INT_EQUAL(tc, false, ogs_sbi_session_operation_result(body, &parsed));
    cJSON_ReplaceItemInObjectCaseSensitive(body, "state", cJSON_CreateString("unknown"));
    ABTS_INT_EQUAL(tc, true, ogs_sbi_session_operation_result(body, &parsed));
    cJSON_Delete(body);
    ABTS_TRUE(tc, ogs_sbi_session_control_id("d7ab8a5c-9723-45ed-ae47-21c41889fd63", true));
    ABTS_TRUE(tc, !ogs_sbi_session_control_id("d7ab8a5c/9723-45ed-ae47-21c41889fd63", true));
    ABTS_TRUE(tc, !ogs_sbi_session_control_id("short", false));
}

static void connected_test(abts_case *tc, void *data)
{
    smf_sess_t sess = {0};
    smf_bearer_t bearer = {0};
    ogs_pfcp_far_t far = {0};
    ogs_list_init(&sess.bearer_list);
    bearer.qfi = 4; bearer.dl_far = &far;
    ogs_list_add(&sess.bearer_list, &bearer);
    sess.remote_dl_teid = 42; far.apply_action = OGS_PFCP_APPLY_ACTION_FORW;
    ABTS_INT_EQUAL(tc, true, smf_session_control_connected(&sess));
    sess.up_cnx_state = OpenAPI_up_cnx_state_ACTIVATED;
    ABTS_INT_EQUAL(tc, true, smf_session_control_connected(&sess));
    sess.up_cnx_state = OpenAPI_up_cnx_state_ACTIVATING;
    ABTS_INT_EQUAL(tc, false, smf_session_control_connected(&sess));
    sess.up_cnx_state = OpenAPI_up_cnx_state_DEACTIVATED;
    ABTS_INT_EQUAL(tc, false, smf_session_control_connected(&sess));
    sess.up_cnx_state = OpenAPI_up_cnx_state_NULL;
    far.apply_action = OGS_PFCP_APPLY_ACTION_BUFF;
    ABTS_INT_EQUAL(tc, false, smf_session_control_connected(&sess));
    far.apply_action = OGS_PFCP_APPLY_ACTION_FORW; sess.n2_released = true;
    ABTS_INT_EQUAL(tc, false, smf_session_control_connected(&sess));
    sess.n2_released = false; sess.remote_dl_teid = 0;
    ABTS_INT_EQUAL(tc, false, smf_session_control_connected(&sess));
}

abts_suite *test_session_control(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, bitrate_test, NULL);
    abts_run_test(suite, config_test, NULL);
    abts_run_test(suite, operation_test, NULL);
    abts_run_test(suite, connected_test, NULL);
    return suite;
}
