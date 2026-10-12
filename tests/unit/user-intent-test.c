/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/smf/user-intent.h"
#include "core/abts.h"

static void parse_test(abts_case *tc, void *data)
{
    uint8_t packet[] = {0x80,0xff,0x10,6,0x64,0xf0,0,1,2,2};
    uint8_t combined[] = {0x80,0,0x0d,0,0xff,0x10,6,0x64,0xf0,0,1,2,2};
    uint8_t duplicate[] = {0x80,0xff,0x10,6,0x64,0xf0,0,1,2,2,
                            0xff,0x10,6,0x64,0xf0,0,1,2,2};
    ogs_plmn_id_t plmn;
    smf_intent_value_t v = {0};
    unsigned i;
    ogs_log_install_domain(&__smf_log_domain, "smf", OGS_LOG_FATAL);
    ogs_plmn_id_build(&plmn, 460, 0, 2);
    ABTS_INT_EQUAL(tc, OGS_OK, smf_intent_parse(packet, sizeof(packet), 0xff10, &plmn, &v));
    ABTS_INT_EQUAL(tc, 1, v.comm_status);
    ABTS_INT_EQUAL(tc, 2, v.ai_model);
    ABTS_INT_EQUAL(tc, 2, v.gpu_level);
    ABTS_INT_EQUAL(tc, OGS_OK, smf_intent_parse(combined, sizeof(combined), 0xff10, &plmn, &v));
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(duplicate, sizeof(duplicate), 0xff10, &plmn, &v));
    ABTS_INT_EQUAL(tc, OGS_DONE, smf_intent_parse(packet, sizeof(packet), 0xff11, &plmn, &v));
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(NULL, 0, 0xff10, &plmn, &v));
    for (i = 2; i < sizeof(packet); i++)
        ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(packet, i, 0xff10, &plmn, &v));
    packet[3] = 255;
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(packet, sizeof(packet), 0xff10, &plmn, &v));
    packet[3] = 6;
    for (i = 7; i < 10; i++) {
        uint8_t saved = packet[i];
        packet[i] = 255;
        ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(packet, sizeof(packet), 0xff10, &plmn, &v));
        packet[i] = saved;
    }
    ogs_plmn_id_build(&plmn, 460, 0, 3);
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_intent_parse(packet, sizeof(packet), 0xff10, &plmn, &v));
    ogs_plmn_id_build(&plmn, 460, 0, 2);
    packet[7] = 0; packet[8] = 1; packet[9] = 3;
    ABTS_INT_EQUAL(tc, OGS_OK, smf_intent_parse(packet, sizeof(packet), 0xff10, &plmn, &v));
    ABTS_INT_EQUAL(tc, 0, v.comm_status);
}

static void standard_pco_test(abts_case *tc, void *data)
{
    uint8_t request[] = {0x80,0,0x0d,0,0xff,0x10,6,0x64,0xf0,0,1,2,2};
    uint8_t pap[] = {0x80,0xc0,0x23,0};
    uint8_t ipcp[] = {0x80,0x80,0x21,4,1,1,0,255};
    uint8_t result[OGS_MAX_PCO_LEN];
    ogs_pco_t pco;
    int length;
    smf_self()->user_intent.container_id = 0xff10;
    smf_self()->dns[0] = "8.8.8.8";
    smf_self()->dns[1] = NULL;
    length = smf_intent_epco_response(result, sizeof(result), request, sizeof(request));
    ABTS_INT_EQUAL(tc, 8, length);
    ABTS_INT_EQUAL(tc, length, ogs_pco_parse(&pco, result, length));
    ABTS_INT_EQUAL(tc, 1, pco.num_of_id);
    ABTS_INT_EQUAL(tc, OGS_PCO_ID_DNS_SERVER_IPV4_ADDRESS_REQUEST, pco.ids[0].id);
    ABTS_TRUE(tc, !memcmp(pco.ids[0].data, "\x08\x08\x08\x08", 4));
    ABTS_INT_EQUAL(tc, 0, smf_intent_epco_response(result, 1, request, sizeof(request)));
    ABTS_INT_EQUAL(tc, 0, smf_intent_epco_response(result, sizeof(result), pap, sizeof(pap)));
    ABTS_INT_EQUAL(tc, 0, smf_intent_epco_response(result, sizeof(result), ipcp, sizeof(ipcp)));
    smf_self()->dns[0] = NULL;
}

static void policy_test(abts_case *tc, void *data)
{
    smf_intent_value_t v = {1,2,2};
    smf_intent_plan_t p = smf_intent_plan(&v, false, 0, 0, 40);
    unsigned i;
    ABTS_INT_EQUAL(tc, SMF_INTENT_CREATE, p.operation);
    ABTS_INT_EQUAL(tc, 2, p.five_qi);
    ABTS_INT_EQUAL(tc, 40, p.priority);
    for (i = 40; i > 1; i--) {
        p = smf_intent_plan(&v, true, 2, i, 40);
        ABTS_INT_EQUAL(tc, SMF_INTENT_MODIFY, p.operation);
        ABTS_INT_EQUAL(tc, i-1, p.priority);
    }
    p = smf_intent_plan(&v, true, 2, 1, 40);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
    ABTS_INT_EQUAL(tc, 1, p.priority);
    v.ai_model = 3;
    p = smf_intent_plan(&v, true, 2, 1, 30);
    ABTS_INT_EQUAL(tc, 3, p.five_qi);
    ABTS_INT_EQUAL(tc, 30, p.priority);
    v.ai_model = 1;
    p = smf_intent_plan(&v, true, 3, 29, 30);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
    ABTS_INT_EQUAL(tc, 29, p.priority);
    ABTS_INT_EQUAL(tc, 3, p.five_qi);
    v.ai_model = 2;
    p = smf_intent_plan(&v, true, 3, 29, 40);
    ABTS_INT_EQUAL(tc, 40, p.priority);
    p = smf_intent_plan(&v, false, 0, 0, 55);
    ABTS_INT_EQUAL(tc, 55, p.priority);
    p = smf_intent_plan(&v, true, 3, 29, 55);
    ABTS_INT_EQUAL(tc, 55, p.priority);
    p = smf_intent_plan(&v, true, 2, 55, 55);
    ABTS_INT_EQUAL(tc, 54, p.priority);
    v.ai_model = 3;
    p = smf_intent_plan(&v, true, 2, 54, 25);
    ABTS_INT_EQUAL(tc, 25, p.priority);
    p = smf_intent_plan(&v, false, 0, 0, 1);
    ABTS_INT_EQUAL(tc, SMF_INTENT_CREATE, p.operation);
    ABTS_INT_EQUAL(tc, 1, p.priority);
    v.comm_status = 0;
    p = smf_intent_plan(&v, true, 3, 29, 25);
    ABTS_INT_EQUAL(tc, SMF_INTENT_DELETE, p.operation);
    p = smf_intent_plan(&v, false, 0, 0, 25);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
}

static void config_test(abts_case *tc, void *data)
{
    smf_context_t *c = smf_self();
    smf_intent_qos_config_t face = {
        55, "120 Mbps", "80 Mbps", "12 Mbps", "8 Mbps"};
    const char *invalid_rates[] = {NULL, "", "500", "5 mbps", "0 Mbps",
        "-1 Mbps", "NaN Mbps", "inf Mbps", "1e999 Mbps", "4.1 Tbps",
        "0.1 bps", "5 Mbps junk"};
    const char *valid_rates[] = {"12000000 bps", "12000 Kbps", "12 Mbps",
        ".012 Gbps", "0.000012 Tbps"};
    const char *invalid_yaml[] = {
        "face_recognition: 55\n", "face_recognition: [55]\n",
        "face_recognition: {priority_level: [55]}\n",
        "face_recognition: {priority_level: 0}\n",
        "face_recognition: {priority_level: 128}\n",
        "face_recognition: {priority_level: 257}\n",
        "face_recognition: {priority_level: 999999999999999999999}\n",
        "face_recognition: {priority_level: 1.5}\n"};
    const char *valid_yaml = "face_recognition: {priority_level: 127, "
        "maxbr_dl: 200 Mbps, gbr_ul: 1.5 Mbps}\n";
    size_t i;
    c->user_intent.enabled = true;
    c->user_intent.mcc = "460";
    c->user_intent.mnc = "00";
    c->user_intent.notify_uri = "http://127.0.0.1:8000/api/v1/intent/notify";
    c->user_intent.face_recognition = face;
    c->user_intent.object_recognition = (smf_intent_qos_config_t){
        25, "240 Mbps", "160 Mbps", "24 Mbps", "16 Mbps"};
    ABTS_INT_EQUAL(tc, OGS_OK, smf_user_intent_config_validate());
    for (i = 0; i < OGS_ARRAY_SIZE(invalid_rates); i++) {
        c->user_intent.face_recognition.maxbr_dl = invalid_rates[i];
        ABTS_INT_EQUAL(tc, OGS_ERROR, smf_user_intent_config_validate());
    }
    c->user_intent.face_recognition = face;
    for (i = 0; i < OGS_ARRAY_SIZE(valid_rates); i++) {
        c->user_intent.face_recognition.gbr_dl = valid_rates[i];
        ABTS_INT_EQUAL(tc, OGS_OK, smf_user_intent_config_validate());
    }
    c->user_intent.face_recognition.priority_level = 128;
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_user_intent_config_validate());
    c->user_intent.face_recognition = face;
    c->user_intent.face_recognition.gbr_dl = "121 Mbps";
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_user_intent_config_validate());
    c->user_intent.face_recognition = face;
    c->user_intent.object_recognition.gbr_ul = "161 Mbps";
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_user_intent_config_validate());

    for (i = 0; i <= OGS_ARRAY_SIZE(invalid_yaml); i++) {
        yaml_parser_t parser;
        yaml_document_t document;
        ogs_yaml_iter_t it;
        const char *input = i == OGS_ARRAY_SIZE(invalid_yaml) ?
            valid_yaml : invalid_yaml[i];
        smf_intent_qos_config_t parsed = face;
        ABTS_TRUE(tc, yaml_parser_initialize(&parser));
        yaml_parser_set_input_string(&parser,
                (const unsigned char *)input, strlen(input));
        ABTS_TRUE(tc, yaml_parser_load(&parser, &document));
        ogs_yaml_iter_init(&it, &document);
        ABTS_TRUE(tc, ogs_yaml_iter_next(&it));
        ABTS_INT_EQUAL(tc, i == OGS_ARRAY_SIZE(invalid_yaml) ? OGS_OK : OGS_ERROR,
                smf_user_intent_qos_config_parse(&it, &parsed));
        if (i == OGS_ARRAY_SIZE(invalid_yaml)) {
            ABTS_INT_EQUAL(tc, 127, parsed.priority_level);
            ABTS_STR_EQUAL(tc, "200 Mbps", parsed.maxbr_dl);
            ABTS_STR_EQUAL(tc, "80 Mbps", parsed.maxbr_ul);
            ABTS_STR_EQUAL(tc, "1.5 Mbps", parsed.gbr_ul);
        }
        yaml_document_delete(&document);
        yaml_parser_delete(&parser);
    }
    memset(&c->user_intent, 0, sizeof(c->user_intent));
}

static void json_test(abts_case *tc, void *data)
{
    smf_intent_value_t v = {1,1,3};
    cJSON *body = smf_intent_notification(&v, "10.45.0.2", "imsi-460001234567890", true, 2);
    ABTS_STR_EQUAL(tc, "010103", cJSON_GetObjectItem(body, "rawPco")->valuestring);
    ABTS_INT_EQUAL(tc, 2, cJSON_GetObjectItem(body, "fiveQi")->valueint);
    ABTS_TRUE(tc, cJSON_GetObjectItem(body, "timestamp")->valuedouble > 1700000000000.0);
    cJSON_Delete(body);
    body = smf_intent_notification(&v, "10.45.0.2", "imsi-460001234567890", false, 2);
    ABTS_TRUE(tc, !cJSON_HasObjectItem(body, "fiveQi"));
    ABTS_STR_EQUAL(tc, "FAILURE", cJSON_GetObjectItem(body, "qosResult")->valuestring);
    cJSON_Delete(body);
    v.comm_status = 0;
    body = smf_intent_notification(&v, "10.45.0.2", "imsi-460001234567890", true, 2);
    ABTS_TRUE(tc, !cJSON_HasObjectItem(body, "fiveQi"));
    ABTS_STR_EQUAL(tc, "000103", cJSON_GetObjectItem(body, "rawPco")->valuestring);
    cJSON_Delete(body);
}

abts_suite *test_user_intent(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, parse_test, NULL);
    abts_run_test(suite, policy_test, NULL);
    abts_run_test(suite, config_test, NULL);
    abts_run_test(suite, standard_pco_test, NULL);
    abts_run_test(suite, json_test, NULL);
    return suite;
}
