/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/smf/user-intent.h"
#include "core/abts.h"
#include "ipfw/ogs-ipfw.h"

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
    smf_intent_qos_config_t face = {.five_qi = 2, .priority_level = 40};
    smf_intent_qos_config_t object = {.five_qi = 3, .priority_level = 30};
    smf_intent_value_t v = {1,2,2};
    smf_intent_plan_t p = smf_intent_plan(&v, false, 0, 0, 0, &face);
    unsigned i;
    ABTS_INT_EQUAL(tc, SMF_INTENT_CREATE, p.operation);
    ABTS_INT_EQUAL(tc, 2, p.five_qi);
    ABTS_INT_EQUAL(tc, 40, p.priority);
    for (i = 40; i > 1; i--) {
        p = smf_intent_plan(&v, true, 2, 2, i, &face);
        ABTS_INT_EQUAL(tc, SMF_INTENT_MODIFY, p.operation);
        ABTS_INT_EQUAL(tc, i-1, p.priority);
    }
    p = smf_intent_plan(&v, true, 2, 2, 1, &face);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
    ABTS_INT_EQUAL(tc, 1, p.priority);
    v.ai_model = 3;
    p = smf_intent_plan(&v, true, 2, 2, 1, &object);
    ABTS_INT_EQUAL(tc, 3, p.five_qi);
    ABTS_INT_EQUAL(tc, 30, p.priority);
    v.ai_model = 1;
    p = smf_intent_plan(&v, true, 3, 3, 29, NULL);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
    ABTS_INT_EQUAL(tc, 29, p.priority);
    ABTS_INT_EQUAL(tc, 3, p.five_qi);
    v.ai_model = 2;
    face.five_qi = 1; face.priority_level = 55;
    p = smf_intent_plan(&v, false, 0, 0, 0, &face);
    ABTS_INT_EQUAL(tc, 1, p.five_qi);
    ABTS_INT_EQUAL(tc, 55, p.priority);
    p = smf_intent_plan(&v, true, 3, 3, 29, &face);
    ABTS_INT_EQUAL(tc, 1, p.five_qi);
    ABTS_INT_EQUAL(tc, 55, p.priority);
    p = smf_intent_plan(&v, true, 2, 1, 55, &face);
    ABTS_INT_EQUAL(tc, 54, p.priority);
    /* An authorized 5QI change must be restored to this model's profile. */
    p = smf_intent_plan(&v, true, 2, 4, 54, &face);
    ABTS_INT_EQUAL(tc, 1, p.five_qi);
    ABTS_INT_EQUAL(tc, 55, p.priority);
    v.ai_model = 3;
    object.five_qi = 4; object.priority_level = 25;
    p = smf_intent_plan(&v, true, 2, 1, 54, &object);
    ABTS_INT_EQUAL(tc, 4, p.five_qi);
    ABTS_INT_EQUAL(tc, 25, p.priority);
    /* Equal configured 5QIs still select the new model's priority. */
    object.five_qi = 1;
    p = smf_intent_plan(&v, true, 2, 1, 54, &object);
    ABTS_INT_EQUAL(tc, 25, p.priority);
    p = smf_intent_plan(&v, true, 3, 1, 25, &object);
    ABTS_INT_EQUAL(tc, 24, p.priority);
    object.five_qi = 255; object.priority_level = 1;
    p = smf_intent_plan(&v, false, 0, 0, 0, &object);
    ABTS_INT_EQUAL(tc, SMF_INTENT_CREATE, p.operation);
    ABTS_INT_EQUAL(tc, 255, p.five_qi);
    ABTS_INT_EQUAL(tc, 1, p.priority);
    v.comm_status = 0;
    p = smf_intent_plan(&v, true, 3, 1, 29, NULL);
    ABTS_INT_EQUAL(tc, SMF_INTENT_DELETE, p.operation);
    p = smf_intent_plan(&v, false, 0, 0, 0, NULL);
    ABTS_INT_EQUAL(tc, SMF_INTENT_NONE, p.operation);
}

static void config_test(abts_case *tc, void *data)
{
    smf_context_t *c = smf_self();
    smf_intent_qos_config_t face = {
        1, 55, "120 Mbps", "80 Mbps", "12 Mbps", "8 Mbps", 2,
        {"permit out ip from 0.0.0.0/0 to assigned", "permit in ip from assigned to 0.0.0.0/0"}};
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
        "face_recognition: {priority_level: 1.5}\n",
        "face_recognition: {five_qi: 0}\n",
        "face_recognition: {five_qi: 256}\n",
        "face_recognition: {five_qi: -1}\n",
        "face_recognition: {five_qi: 1.5}\n",
        "face_recognition: {five_qi: 999999999999999999999}\n",
        "face_recognition: {five_qi: [1]}\n",
        "face_recognition: {flow_descriptions: []}\n",
        "face_recognition: {flow_descriptions: invalid}\n",
        "face_recognition: {flow_descriptions: {x: y}}\n",
        "face_recognition: {flow_descriptions: [[x]]}\n",
        "face_recognition: {flow_descriptions: [null]}\n",
        "face_recognition: {flow_descriptions: [\"permit in ip from assigned\"]}\n",
        "face_recognition: {flow_descriptions: [\"permit out ip from 999.1.1.1 to assigned\"]}\n"};
    const char *valid_yaml = "face_recognition: {five_qi: 255, priority_level: 127, "
        "maxbr_dl: 200 Mbps, gbr_ul: 1.5 Mbps, flow_descriptions: ["
        "\"permit out tcp from 10.2.0.102/32 8000 to assigned\","
        "\"permit in tcp from assigned to 10.2.0.102/32 8000\"]}\n";
    size_t i;
    c->user_intent.enabled = true;
    c->user_intent.mcc = "460";
    c->user_intent.mnc = "00";
    c->user_intent.notify_uri = "http://127.0.0.1:8000/api/v1/intent/notify";
    c->user_intent.face_recognition = face;
    c->user_intent.object_recognition = (smf_intent_qos_config_t){
        4, 25, "240 Mbps", "160 Mbps", "24 Mbps", "16 Mbps", 2,
        {"permit out ip from 0.0.0.0/0 to assigned", "permit in ip from assigned to 0.0.0.0/0"}};
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
    c->user_intent.face_recognition.five_qi = 256;
    ABTS_INT_EQUAL(tc, OGS_ERROR, smf_user_intent_config_validate());
    c->user_intent.face_recognition = face;
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
            ABTS_INT_EQUAL(tc, 255, parsed.five_qi);
            ABTS_INT_EQUAL(tc, 127, parsed.priority_level);
            ABTS_STR_EQUAL(tc, "200 Mbps", parsed.maxbr_dl);
            ABTS_STR_EQUAL(tc, "80 Mbps", parsed.maxbr_ul);
            ABTS_STR_EQUAL(tc, "1.5 Mbps", parsed.gbr_ul);
            ABTS_INT_EQUAL(tc, 2, parsed.num_of_flow);
            ABTS_STR_EQUAL(tc, "permit out tcp from 10.2.0.102/32 8000 to assigned", parsed.flow_descriptions[0]);
        }
        yaml_document_delete(&document);
        yaml_parser_delete(&parser);
    }
    /* Exercise the packet-filter limit at the YAML boundary. */
    for (i = OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE;
            i <= OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE + 1; i++) {
        yaml_parser_t parser;
        yaml_document_t document;
        ogs_yaml_iter_t it;
        smf_intent_qos_config_t parsed = face;
        char input[2048], *p = input;
        size_t j;
        p = ogs_slprintf(p, input + sizeof(input), "face_recognition:\n  flow_descriptions:\n");
        for (j = 0; j < i; j++)
            p = ogs_slprintf(p, input + sizeof(input), "    - \"permit out ip from 0.0.0.0/0 to assigned\"\n");
        ABTS_TRUE(tc, yaml_parser_initialize(&parser));
        yaml_parser_set_input_string(&parser, (const unsigned char *)input, strlen(input));
        ABTS_TRUE(tc, yaml_parser_load(&parser, &document));
        ogs_yaml_iter_init(&it, &document);
        ABTS_TRUE(tc, ogs_yaml_iter_next(&it));
        ABTS_INT_EQUAL(tc, i == OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE ? OGS_OK : OGS_ERROR,
                smf_user_intent_qos_config_parse(&it, &parsed));
        yaml_document_delete(&document);
        yaml_parser_delete(&parser);
    }
    memset(&c->user_intent, 0, sizeof(c->user_intent));
}

static void flow_test(abts_case *tc, void *data)
{
    const char *valid[] = {
        "permit out ip from 0.0.0.0/0 to assigned",
        "permit in ip from assigned to 0.0.0.0/0",
        "permit out tcp from 10.2.0.102/32 8000 to assigned 10000-10010",
        "permit in tcp from assigned 10000-10010 to 10.2.0.102/32 8000",
        "permit out udp from 2001:db8::1/128 5000 to assigned",
        "permit out 17 from any to assigned",
        "permit  in  ip  from  assigned  to  192.0.2.1/32"};
    const char *invalid[] = {NULL, "", " ", "permit", "deny out ip from any to assigned",
        "permit in ip from assigned", "permit out ip from any", "permit out ip from any to",
        "permit out unknown from any to assigned", "permit out 256 from any to assigned",
        "permit out ip from 256.1.1.1 to assigned", "permit out ip from 192.0.2.1/33 to assigned",
        "permit out udp from 2001:db8::1/129 to assigned", "permit out tcp from any 65536 to assigned",
        "permit out tcp from any 100-99 to assigned", "permit out tcp from any 1-2-3 to assigned",
        "permit out ip from assigned to any", "permit in ip from any to assigned",
        "permit out ip from 192.0.2.1 to 2001:db8::1", "permit out ip from any to assigned extra junk"};
    ogs_pcc_rule_t rule = {0};
    smf_intent_qos_config_t profile = {.num_of_flow = 2,
        .flow_descriptions = {valid[2], valid[3]}};
    char long_description[513];
    unsigned i;
    for (i = 0; i < OGS_ARRAY_SIZE(valid); i++) {
        ogs_flow_t flow = {0};
        ogs_ipfw_rule_t ipfw;
        ABTS_INT_EQUAL(tc, OGS_OK, ogs_flow_description_parse(&flow, valid[i]));
        ABTS_INT_EQUAL(tc, OGS_OK, ogs_ipfw_compile_rule(&ipfw, flow.description));
        if (i == 1) {
            ABTS_INT_EQUAL(tc, OGS_FLOW_UPLINK_ONLY, flow.direction);
            ABTS_STR_EQUAL(tc, valid[0], flow.description);
        }
        if (i == 3) ABTS_STR_EQUAL(tc, valid[2], flow.description);
        OGS_FLOW_FREE(&flow);
    }
    for (i = 0; i < OGS_ARRAY_SIZE(invalid); i++) {
        ogs_flow_t flow = {.direction = 99};
        ABTS_INT_EQUAL(tc, OGS_ERROR, ogs_flow_description_parse(&flow, invalid[i]));
        ABTS_INT_EQUAL(tc, 99, flow.direction);
        ABTS_TRUE(tc, flow.description == NULL);
    }
    memset(long_description, 'x', sizeof(long_description) - 1);
    long_description[sizeof(long_description) - 1] = 0;
    ABTS_INT_EQUAL(tc, OGS_ERROR, ogs_flow_description_parse(&rule.flow[0], long_description));
    for (i = 0; i < 2; i++) {
        ABTS_INT_EQUAL(tc, OGS_OK, ogs_flow_description_parse(&rule.flow[i], profile.flow_descriptions[i]));
        rule.num_of_flow++;
    }
    ABTS_TRUE(tc, smf_intent_flows_match(&rule, &profile));
    { ogs_flow_t saved = rule.flow[0]; rule.flow[0] = rule.flow[1]; rule.flow[1] = saved; }
    ABTS_TRUE(tc, smf_intent_flows_match(&rule, &profile));
    profile.flow_descriptions[0] = valid[0];
    ABTS_TRUE(tc, !smf_intent_flows_match(&rule, &profile));
    profile.num_of_flow = OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE + 1;
    ABTS_TRUE(tc, !smf_intent_flows_match(&rule, &profile));
    OGS_PCC_RULE_FREE(&rule);
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
    body = smf_intent_notification(&v, "10.45.0.2", "imsi-460001234567890", true, 255);
    ABTS_INT_EQUAL(tc, 255, cJSON_GetObjectItem(body, "fiveQi")->valueint);
    cJSON_Delete(body);
    body = smf_intent_notification(&v, "10.45.0.2", "imsi-460001234567890", true, 0);
    ABTS_TRUE(tc, !cJSON_HasObjectItem(body, "fiveQi"));
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
    abts_run_test(suite, flow_test, NULL);
    return suite;
}
