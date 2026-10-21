/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/smf/ue-events.h"

static const char *argument(void *request, const char *key)
{
    return !strcmp(key, "detail") ? "1" : NULL;
}

static const cJSON *get(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static cJSON *events(void)
{
    int status = 0;
    char *text = ogs_ue_events_query(argument, NULL, &status);
    ogs_assert(status == 200 && text);
    cJSON *root = cJSON_Parse(text);
    free(text);
    ogs_assert(root);
    return root;
}

static void check_last(const char *name, const char *direction, const char *result, const char *info)
{
    cJSON *root = events();
    const cJSON *items = get(root, "items");
    const cJSON *event = cJSON_GetArrayItem(items, cJSON_GetArraySize(items)-1);
    ogs_assert(event);
    if (strcmp(get(event, "name")->valuestring, name) ||
            strcmp(get(event, "direction")->valuestring, direction) ||
            strcmp(get(event, "result")->valuestring, result) ||
            strcmp(get(event, "info")->valuestring, info))
        fprintf(stderr, "Expected %s %s %s [%s], got %s %s %s [%s]\n",
                name, direction, result, info, get(event, "name")->valuestring,
                get(event, "direction")->valuestring, get(event, "result")->valuestring,
                get(event, "info")->valuestring);
    ogs_assert(!strcmp(get(event, "name")->valuestring, name));
    ogs_assert(!strcmp(get(event, "direction")->valuestring, direction));
    ogs_assert(!strcmp(get(event, "result")->valuestring, result));
    ogs_assert(!strcmp(get(event, "info")->valuestring, info));
    ogs_assert(get(event, "psi")->valueint == 2);
    cJSON_Delete(root);
}

static unsigned count_completed(void)
{
    unsigned count = 0;
    cJSON *root = events();
    const cJSON *event;
    cJSON_ArrayForEach(event, get(root, "items")) {
        if (!strcmp(get(event, "name")->valuestring, "SessionEstablishment") &&
                !strcmp(get(event, "result")->valuestring, "completed")) count++;
    }
    cJSON_Delete(root);
    return count;
}

static void test_sbi(smf_sess_t *sess)
{
    const struct {
        const char *service, *method, *resource[4];
        int status;
        const char *name, *direction, *info;
    } cases[] = {
        {"nsmf-pdusession", "POST", {"sm-contexts"}, 0,
            "CreateSMContextRequest", "AMF->SMF", "POST /nsmf-pdusession/v1/sm-contexts"},
        {"nsmf-pdusession", "POST", {"sm-contexts", "1", "modify"}, 0,
            "UpdateSMContextRequest", "AMF->SMF", "POST /nsmf-pdusession/v1/sm-contexts/1/modify"},
        {"nsmf-pdusession", "POST", {"sm-contexts", "1", "release"}, 0,
            "ReleaseSMContextRequest", "AMF->SMF", "POST /nsmf-pdusession/v1/sm-contexts/1/release"},
        {"nudm-sdm", "GET", {"imsi-001010000000001", "sm-data"}, 200,
            "GetSMDataResponse", "UDM->SMF", "GET /nudm-sdm/v1/imsi-001010000000001/sm-data HTTP=200"},
        {"nudm-sdm", "POST", {"imsi-001010000000001", "sdm-subscriptions"}, 201,
            "CreateSDMSubscriptionResponse", "UDM->SMF", "POST /nudm-sdm/v1/imsi-001010000000001/sdm-subscriptions HTTP=201"},
        {"nudm-uecm", "PUT", {"imsi-001010000000001", "registrations", "smf-registrations", "2"}, 200,
            "RegisterSMFResponse", "UDM->SMF", "PUT /nudm-uecm/v1/imsi-001010000000001/registrations/smf-registrations/2 HTTP=200"},
        {"npcf-smpolicycontrol", "POST", {"sm-policies"}, 201,
            "CreateSMPolicyResponse", "PCF->SMF", "POST /npcf-smpolicycontrol/v1/sm-policies HTTP=201"},
        {"npcf-smpolicycontrol", "POST", {"sm-policies", "1", "delete"}, 204,
            "DeleteSMPolicyResponse", "PCF->SMF", "POST /npcf-smpolicycontrol/v1/sm-policies/1/delete HTTP=204"},
        {"namf-comm", "POST", {"ue-contexts", "imsi-001010000000001", "n1-n2-messages"}, 200,
            "N1N2MessageTransferResponse", "AMF->SMF", "POST /namf-comm/v1/ue-contexts/imsi-001010000000001/n1-n2-messages HTTP=200"},
        {"nsmf-pdusession", "POST", {"pdu-sessions", "ref", "modify"}, 200,
            "UpdatePDUSessionResponse", "SMF->SMF", "POST /nsmf-pdusession/v1/pdu-sessions/ref/modify HTTP=200"},
        {"nudm-sdm", "GET", {"imsi-001010000000001", "sm-data"}, 404,
            "GetSMDataResponse", "UDM->SMF", "GET /nudm-sdm/v1/imsi-001010000000001/sm-data HTTP=404"}
    };
    for (size_t i = 0; i < OGS_ARRAY_SIZE(cases); i++) {
        ogs_sbi_message_t message = {0};
        message.h.method = (char *)cases[i].method;
        message.h.service.name = (char *)cases[i].service;
        message.h.api.version = (char *)"v1";
        for (size_t j = 0; j < OGS_ARRAY_SIZE(cases[i].resource); j++)
            message.h.resource.component[j] = (char *)cases[i].resource[j];
        message.res_status = cases[i].status;
        smf_ue_event_sbi(sess, &message, true);
        check_last(cases[i].name, cases[i].direction,
                cases[i].status >= 400 ? "failed" : "received", cases[i].info);
    }
    /* Logging must not free or modify the borrowed URI/method. */
    ogs_sbi_request_t request = {0};
    request.h.method = (char *)"POST";
    request.h.uri = (char *)"http://127.0.0.5:7777/namf-comm/v1/ue-contexts/imsi-001010000000001/n1-n2-messages";
    smf_ue_event_sbi_request(sess, &request, "queued");
    check_last("N1N2MessageTransferRequest", "SMF->AMF", "queued",
            "POST /namf-comm/v1/ue-contexts/imsi-001010000000001/n1-n2-messages");
    smf_ue_event_sbi_request(sess, &request, "timeout");
    check_last("N1N2MessageTransferRequest", "SMF->AMF", "timeout",
            "POST /namf-comm/v1/ue-contexts/imsi-001010000000001/n1-n2-messages");
    request.h.uri = (char *)"http://127.0.0.5:7777";
    smf_ue_event_sbi_request(sess, &request, "send-failed");
    check_last("SBIRequest", "SMF->peer", "send-failed", "Invalid SBI request URI");
    request.h.uri = (char *)"/npcf-smpolicycontrol/v1/sm-policies/1/update";
    smf_ue_event_sbi_request(sess, &request, "queued");
    check_last("UpdateSMPolicyRequest", "SMF->PCF", "queued", "POST /npcf-smpolicycontrol/v1/sm-policies/1/update");

    char component[1024];
    memset(component, 'a', sizeof(component)-1); component[sizeof(component)-1] = 0;
    request.h.service.name = (char *)"nudm-uecm";
    request.h.api.version = (char *)"v1";
    request.h.resource.component[0] = component;
    for (size_t i = 1; i < OGS_SBI_MAX_NUM_OF_RESOURCE_COMPONENT; i++)
        request.h.resource.component[i] = (char *)"tail";
    smf_ue_event_sbi_request(sess, &request, "queued");
    cJSON *root = events();
    const cJSON *items = get(root, "items");
    const cJSON *event = cJSON_GetArrayItem(items, cJSON_GetArraySize(items)-1);
    ogs_assert(strlen(get(event, "info")->valuestring) == sizeof(((ogs_ue_event_t *)0)->info)-1);
    cJSON_Delete(root);
}

static void test_establishment(smf_sess_t *sess)
{
    ogs_nas_5gs_message_t message = {0};
    message.gsm.h.message_type = OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REQUEST;
    message.gsm.h.pdu_session_identity = 2;
    smf_ue_event_nas(sess, &message, true);
    uint64_t flow_id = sess->event_flow_id;
    ogs_assert(sess->event_establishment.pending);
    smf_ue_event_establishment_n4(sess, 0, "completed", "invalid");
    smf_ue_event_establishment_n4(sess, 100, "completed", "unrelated");
    ogs_assert(!count_completed() && sess->event_establishment.pending);
    sess->event_establishment.xact_id = 42;
    sess->event_flow_id = ogs_ue_events_new_trace();
    smf_ue_event_establishment_n4(sess, 43, "completed", "QoS or handover");
    ogs_assert(!count_completed());
    smf_ue_event_establishment_n4(sess, 42, "completed", "Initial N2 setup and N4 activation completed");
    smf_ue_event_establishment_n4(sess, 42, "completed", "duplicate");
    ogs_assert(count_completed() == 1 && !sess->event_establishment.pending);
    check_last("SessionEstablishment", "LOCAL", "completed", "Initial N2 setup and N4 activation completed");
    cJSON *root = events();
    const cJSON *items = get(root, "items");
    const cJSON *event = cJSON_GetArrayItem(items, cJSON_GetArraySize(items)-1);
    ogs_assert(strtoull(get(event, "flow_id")->valuestring, NULL, 10) == flow_id);
    cJSON_Delete(root);
    const char *outcomes[] = {"failed", "timeout", "cancelled"};
    for (size_t i = 0; i < OGS_ARRAY_SIZE(outcomes); i++) {
        smf_ue_event_nas(sess, &message, true);
        sess->event_establishment.xact_id = 42;
        smf_ue_event_establishment_n4(sess, 42, outcomes[i], "initial setup did not complete");
        smf_ue_event_establishment_n4(sess, 42, "completed", "late response");
        ogs_assert(count_completed() == 1 && !sess->event_establishment.pending);
        check_last("SessionEstablishment", "LOCAL", outcomes[i], "initial setup did not complete");
    }
    smf_ue_event_nas(sess, &message, true);
    sess->event_establishment.xact_id = 42;
    message.gsm.h.message_type = OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REJECT;
    smf_ue_event_nas(sess, &message, false);
    smf_ue_event_establishment_n4(sess, 42, "completed", "late response");
    ogs_assert(count_completed() == 1 && !sess->event_establishment.pending);
}

int main(void)
{
    ogs_core_initialize();
    ogs_log_install_domain(&__ogs_sbi_domain, "sbi", OGS_LOG_ERROR);
    ogs_app_context_init();
    ogs_app_config_init();
    ogs_global_conf()->max.ue = 8;
    ogs_app()->pool.sess = 16;
    ogs_app()->metrics.max_specs = 256;
    smf_metrics_init();
    smf_context_init();
    ogs_assert(ogs_ue_events_init("SMF", 128, 128));
    smf_ue_t *ue = smf_ue_add_by_supi((char *)"imsi-001010000000001");
    ogs_assert(ue);
    smf_sess_t sess = {.smf_ue_id = ue->id, .psi = 2,
        .event_trace_id = ogs_ue_events_new_trace()};
    ogs_list_init(&sess.bearer_list);
    test_sbi(&sess);
    test_establishment(&sess);
    ogs_ue_events_final();
    smf_context_final();
    smf_metrics_final();
    ogs_app_config_final();
    ogs_app_context_final();
    ogs_core_terminate();
    talloc_disable_null_tracking();
    return 0;
}
