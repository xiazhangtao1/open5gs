/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "test-common.h"
#include <curl/curl.h>
#include <poll.h>

/* Real PCF/SMF/AMF/UPF processes, with controlled NGAP/NAS peers.
 * XCN_QOS_PCF_URL and XCN_QOS_TEST_MSIN allow the same test against a
 * deployed core using a config with all local NFs disabled. */
typedef struct {
    char body[65536];
    size_t length;
    char app_id[256];
} http_result_t;

static size_t http_body(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_result_t *result = userdata;
    size_t length = size * nmemb;
    if (length >= sizeof(result->body) - result->length)
        return 0;
    memcpy(result->body + result->length, ptr, length);
    result->length += length;
    result->body[result->length] = 0;
    return length;
}

static size_t http_header(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_result_t *result = userdata;
    size_t length = size * nmemb;
    if (length > 10 && !ogs_strncasecmp(ptr, "location:", 9)) {
        char location[1024], *end, *id;
        ogs_assert(length < sizeof(location));
        memcpy(location, ptr, length);
        location[length] = 0;
        end = strpbrk(location, "\r\n");
        if (end) *end = 0;
        id = strrchr(location, '/');
        ogs_assert(id && strlen(id + 1) < sizeof(result->app_id));
        ogs_cpystrn(result->app_id, id + 1, sizeof(result->app_id));
    }
    return length;
}

static cJSON *request(abts_case *tc, const char *method, const char *path,
        cJSON *body, int expected, char *app_id)
{
    const char *base = getenv("XCN_QOS_PCF_URL");
    http_result_t result = {0};
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    char *url, *encoded = body ? cJSON_PrintUnformatted(body) : NULL;
    long status = 0;
    cJSON *json = NULL;
    ogs_assert(curl);
    if (!base) base = "http://127.0.0.13:7777";
    url = ogs_msprintf("%s/xcn-dedicated-bearer/v1/bearers%s", base, path);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, http_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    if (encoded) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, encoded);
    ogs_assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status != expected)
        fprintf(stderr, "%s %s: HTTP %ld: %s\n", method, path, status, result.body);
    if (tc) ABTS_INT_EQUAL(tc, expected, status);
    ogs_assert(status == expected);
    if (app_id) {
        ogs_assert(result.app_id[0]);
        ogs_cpystrn(app_id, result.app_id, 256);
    }
    if (result.length) {
        json = cJSON_Parse(result.body);
        ogs_assert(json);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    ogs_free(url);
    if (encoded) ogs_free(encoded);
    return json;
}

static void send_ngap(abts_case *tc, ogs_socknode_t *ngap, ogs_pkbuf_t *buf)
{
    ogs_assert(buf);
    ABTS_INT_EQUAL(tc, OGS_OK, testgnb_ngap_send(ngap, buf));
}

static int receive_ngap(abts_case *tc, test_ue_t *ue,
        ogs_socknode_t *ngap, int procedure)
{
    struct pollfd fd = {.fd = ngap->sock->fd, .events = POLLIN};
    ogs_pkbuf_t *buf;
    int psi = 0;
    ogs_assert(poll(&fd, 1, 15000) == 1 && (fd.revents & POLLIN));
    buf = testgnb_ngap_read(ngap);
    if (procedure == NGAP_ProcedureCode_id_PDUSessionResourceModify) {
        ogs_ngap_message_t message = {0};
        NGAP_PDUSessionResourceModifyRequest_t *req;
        int i;
        ogs_assert(ogs_ngap_decode(&message, buf) == OGS_OK);
        ogs_assert(message.present == NGAP_NGAP_PDU_PR_initiatingMessage);
        req = &message.choice.initiatingMessage->value.choice.PDUSessionResourceModifyRequest;
        for (i = 0; i < req->protocolIEs.list.count; i++) {
            NGAP_PDUSessionResourceModifyRequestIEs_t *ie = req->protocolIEs.list.array[i];
            if (ie->id == NGAP_ProtocolIE_ID_id_PDUSessionResourceModifyListModReq) {
                NGAP_PDUSessionResourceModifyListModReq_t *list =
                    &ie->value.choice.PDUSessionResourceModifyListModReq;
                ogs_assert(list->list.count == 1);
                psi = list->list.array[0]->pDUSessionID;
            }
        }
        ogs_ngap_free(&message);
        ogs_assert(psi);
    }
    testngap_recv(ue, buf);
    ABTS_INT_EQUAL(tc, procedure, ue->ngap_procedure_code);
    ogs_assert(ue->ngap_procedure_code == procedure);
    return psi;
}

static void send_sm(abts_case *tc, test_sess_t *sess,
        ogs_socknode_t *ngap, ogs_pkbuf_t *sm)
{
    ogs_pkbuf_t *nas = testgmm_build_ul_nas_transport(sess,
            OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, sm);
    send_ngap(tc, ngap, testngap_build_uplink_nas_transport(sess->test_ue, nas));
}

static cJSON *bearer_body(test_sess_t *sess, int priority)
{
    cJSON *body = cJSON_Parse("{\"qos\":{\"5qi\":2,\"arp\":{\"priorityLevel\":8},"
        "\"maxbrDl\":\"10 Mbps\",\"maxbrUl\":\"10 Mbps\","
        "\"gbrDl\":\"5 Mbps\",\"gbrUl\":\"5 Mbps\"},\"flowDescriptions\":["
        "\"permit out ip from any to assigned\","
        "\"permit in ip from assigned to any\"]}");
    char *supi = ogs_msprintf("imsi-%s", sess->test_ue->imsi);
    ogs_assert(body);
    cJSON_AddStringToObject(body, "supi", supi);
    cJSON_AddNumberToObject(body, "pduSessionId", sess->psi);
    if (priority)
        cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(body, "qos"),
                "priorityLevel", priority);
    ogs_free(supi);
    return body;
}

static void query_bearer(abts_case *tc, test_sess_t *sess,
        const char *app_id, int priority, int count)
{
    char *path = ogs_msprintf("?supi=imsi-%s&pduSessionId=%d",
            sess->test_ue->imsi, sess->psi);
    cJSON *root = request(tc, "GET", path, NULL, 200, NULL);
    cJSON *bearers = cJSON_GetObjectItemCaseSensitive(root, "bearers");
    cJSON *bearer;
    int found = 0;
    ABTS_INT_EQUAL(tc, count, cJSON_GetArraySize(bearers));
    cJSON_ArrayForEach(bearer, bearers) {
        cJSON *id = cJSON_GetObjectItemCaseSensitive(bearer, "appSessionId");
        cJSON *rules, *rule, *qos, *value, *arp;
        if (!app_id || strcmp(id->valuestring, app_id)) continue;
        found++;
        rules = cJSON_GetObjectItemCaseSensitive(bearer, "pccRules");
        rule = cJSON_GetArrayItem(rules, 0);
        ABTS_INT_EQUAL(tc, 100, cJSON_GetObjectItemCaseSensitive(rule, "precedence")->valueint);
        qos = cJSON_GetObjectItemCaseSensitive(rule, "qos");
        value = cJSON_GetObjectItemCaseSensitive(qos, "priorityLevel");
        ABTS_INT_EQUAL(tc, priority != 0, value != NULL);
        if (value) ABTS_INT_EQUAL(tc, priority, value->valueint);
        arp = cJSON_GetObjectItemCaseSensitive(qos, "arp");
        ABTS_INT_EQUAL(tc, 8, cJSON_GetObjectItemCaseSensitive(arp, "priorityLevel")->valueint);
    }
    if (app_id) ABTS_INT_EQUAL(tc, 1, found);
    cJSON_Delete(root);
    ogs_free(path);
}

static test_bearer_t *dedicated_flow(test_sess_t *sess)
{
    test_bearer_t *flow;
    ogs_list_for_each(&sess->bearer_list, flow)
        if (flow->qfi != 1) return flow;
    return NULL;
}

static test_bearer_t *finish_modify(abts_case *tc, test_sess_t *sess,
        ogs_socknode_t *ngap, int priority, bool remove)
{
    test_bearer_t *flow;
    flow = dedicated_flow(sess);
    ogs_assert(flow);
    if (remove) {
        send_ngap(tc, ngap, testngap_build_qos_flow_resource_release_response(flow));
    } else {
        ABTS_INT_EQUAL(tc, 2, flow->qos.index);
        ABTS_INT_EQUAL(tc, priority, flow->qos.priority_level);
        ABTS_INT_EQUAL(tc, 8, flow->qos.arp.priority_level);
        send_ngap(tc, ngap, testngap_build_qos_flow_resource_modify_response(flow));
    }
    sess->ul_nas_transport_param.request_type = OGS_NAS_5GS_REQUEST_TYPE_MODIFICATION_REQUEST;
    sess->ul_nas_transport_param.dnn = 0;
    sess->ul_nas_transport_param.s_nssai = 0;
    send_sm(tc, sess, ngap, testgsm_build_pdu_session_modification_complete(sess));
    /* Give PFCP and the policy notification transaction time to finish. */
    ogs_msleep(40);
    if (remove) test_bearer_remove(flow);
    return remove ? NULL : flow;
}

static test_bearer_t *modify_complete(abts_case *tc, test_sess_t *sess,
        ogs_socknode_t *ngap, int priority, bool remove)
{
    ABTS_INT_EQUAL(tc, sess->psi, receive_ngap(tc, sess->test_ue,
                ngap, NGAP_ProcedureCode_id_PDUSessionResourceModify));
    return finish_modify(tc, sess, ngap, priority, remove);
}

static bson_t *subscriber(test_ue_t *ue)
{
    bson_t *doc = test_db_new_session(ue), *result;
    bson_t array_view, slice_view, slice = BSON_INITIALIZER, array = BSON_INITIALIZER;
    bson_iter_t iter;
    const uint8_t *bytes;
    uint32_t length;
    char *sd = ogs_s_nssai_sd_to_string(test_self()->plmn_support[0].s_nssai[0].sd);
    mongoc_collection_t *collection = mongoc_client_get_collection(
            ogs_mongoc()->client, ogs_mongoc()->name, "subscribers");
    bson_t *key = BCON_NEW("imsi", BCON_UTF8(ue->imsi));
    bson_error_t error;
    /* The shared insert helper replaces subscriptions: never overwrite one. */
    ogs_assert(mongoc_collection_count(collection, MONGOC_QUERY_NONE,
                key, 0, 0, NULL, &error) == 0);
    bson_destroy(key);
    mongoc_collection_destroy(collection);
    if (!sd) return doc;
    ogs_assert(bson_iter_init_find(&iter, doc, "slice"));
    bson_iter_array(&iter, &length, &bytes);
    ogs_assert(bson_init_static(&array_view, bytes, length));
    ogs_assert(bson_iter_init_find(&iter, &array_view, "0"));
    bson_iter_document(&iter, &length, &bytes);
    ogs_assert(bson_init_static(&slice_view, bytes, length));
    bson_copy_to_excluding_noinit(&slice_view, &slice, "sd", NULL);
    BSON_APPEND_UTF8(&slice, "sd", sd);
    BSON_APPEND_DOCUMENT(&array, "0", &slice);
    result = bson_new();
    bson_copy_to_excluding_noinit(doc, result, "slice", NULL);
    BSON_APPEND_ARRAY(result, "slice", &array);
    bson_destroy(&slice);
    bson_destroy(&array);
    bson_destroy(doc);
    ogs_free(sd);
    return result;
}

static test_ue_t *register_ue(abts_case *tc, ogs_socknode_t *ngap)
{
    ogs_nas_5gs_mobile_identity_suci_t identity = {0};
    ogs_pkbuf_t *nas;
    test_ue_t *ue;
    const char *msin = getenv("XCN_QOS_TEST_MSIN");
    identity.h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
    identity.h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
    identity.routing_indicator2 = identity.routing_indicator3 = identity.routing_indicator4 = 0xf;
    if (!msin) msin = "0000000905";
    ue = test_ue_add_by_suci(&identity, msin);
    ogs_assert(ue);
    ue->nr_cgi.cell_id = 0x40001;
    ue->nas.registration.ksi = OGS_NAS_KSI_NO_KEY_IS_AVAILABLE;
    ue->nas.registration.follow_on_request = 1;
    ue->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_INITIAL;
    ue->k_string = "465b5ce8b199b49faa5f0a2ee238a6bc";
    ue->opc_string = "e8ed289deba952e4283b54e88e6183ca";
    send_ngap(tc, ngap, testngap_build_ng_setup_request(0x4095, 22));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_NGSetup);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_insert_ue(ue, subscriber(ue)));
    ue->registration_request_param.guti = 1;
    nas = testgmm_build_registration_request(ue, NULL, false, false);
    send_ngap(tc, ngap, testngap_build_initial_ue_message(ue, nas,
                NGAP_RRCEstablishmentCause_mo_Signalling, false, true));
    ue->registration_request_param.gmm_capability = 1;
    ue->registration_request_param.requested_nssai = 1;
    ue->registration_request_param.last_visited_registered_tai = 1;
    ue->registration_request_param.ue_usage_setting = 1;
    nas = testgmm_build_registration_request(ue, NULL, false, false);
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_DownlinkNASTransport);
    send_ngap(tc, ngap, testngap_build_uplink_nas_transport(ue, testgmm_build_identity_response(ue)));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_DownlinkNASTransport);
    send_ngap(tc, ngap, testngap_build_uplink_nas_transport(ue, testgmm_build_authentication_response(ue)));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_DownlinkNASTransport);
    send_ngap(tc, ngap, testngap_build_uplink_nas_transport(ue, testgmm_build_security_mode_complete(ue, nas)));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_InitialContextSetup);
    send_ngap(tc, ngap, testngap_build_initial_context_setup_response(ue, false));
    send_ngap(tc, ngap, testngap_build_uplink_nas_transport(ue, testgmm_build_registration_complete(ue)));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_DownlinkNASTransport);
    return ue;
}

static test_sess_t *establish_session(abts_case *tc, test_ue_t *ue,
        ogs_socknode_t *ngap, int psi)
{
    test_sess_t *sess = test_sess_add_by_dnn_and_psi(ue, "internet", psi);
    ogs_assert(sess);
    sess->ul_nas_transport_param.request_type = OGS_NAS_5GS_REQUEST_TYPE_INITIAL;
    sess->ul_nas_transport_param.dnn = sess->ul_nas_transport_param.s_nssai = 1;
    sess->pdu_session_establishment_param.ssc_mode = sess->pdu_session_establishment_param.epco = 1;
    send_sm(tc, sess, ngap, testgsm_build_pdu_session_establishment_request(sess));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_PDUSessionResourceSetup);
    send_ngap(tc, ngap, testngap_sess_build_pdu_session_resource_setup_response(sess));
    ogs_msleep(100);
    return sess;
}

static void ping_flow(abts_case *tc, ogs_socknode_t *gtpu, test_bearer_t *flow)
{
    struct pollfd fd = {.fd = gtpu->sock->fd, .events = POLLIN};
    ogs_pkbuf_t *buf;
    ABTS_INT_EQUAL(tc, OGS_OK, test_gtpu_send_ping(gtpu, flow, TEST_PING_IPV4));
    ogs_assert(poll(&fd, 1, 15000) == 1 && (fd.revents & POLLIN));
    buf = testgnb_gtpu_read(gtpu);
    ogs_pkbuf_free(buf);
}

static void recover_access(abts_case *tc, test_sess_t *sess,
        ogs_socknode_t *ngap, int priority)
{
    test_ue_t *ue = sess->test_ue;
    ogs_pkbuf_t *nas;
    send_ngap(tc, ngap, testngap_build_ue_context_release_request(ue,
                NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity, true));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_UEContextRelease);
    send_ngap(tc, ngap, testngap_build_ue_context_release_complete(ue));
    ogs_msleep(100);
    ue->service_request_param.uplink_data_status = 1;
    ue->service_request_param.psimask.uplink_data_status = 1 << sess->psi;
    nas = testgmm_build_service_request(ue, OGS_NAS_SERVICE_TYPE_DATA, NULL, false, false);
    ue->service_request_param.uplink_data_status = 0;
    nas = testgmm_build_service_request(ue, OGS_NAS_SERVICE_TYPE_DATA, nas, true, false);
    send_ngap(tc, ngap, testngap_build_initial_ue_message(ue, nas,
                NGAP_RRCEstablishmentCause_mo_Signalling, true, true));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_InitialContextSetup);
    ABTS_INT_EQUAL(tc, priority, dedicated_flow(sess)->qos.priority_level);
    send_ngap(tc, ngap, testngap_build_initial_context_setup_response(ue, true));
    ogs_msleep(100);
}

static void release_session(abts_case *tc, test_sess_t *sess, ogs_socknode_t *ngap)
{
    memset(&sess->ul_nas_transport_param, 0, sizeof(sess->ul_nas_transport_param));
    send_sm(tc, sess, ngap, testgsm_build_pdu_session_release_request(sess));
    receive_ngap(tc, sess->test_ue, ngap, NGAP_ProcedureCode_id_PDUSessionResourceRelease);
    send_ngap(tc, ngap, testngap_build_pdu_session_resource_release_response(sess));
    send_sm(tc, sess, ngap, testgsm_build_pdu_session_release_complete(sess));
    ogs_msleep(100);
    test_sess_remove(sess);
}

static void priority_lifecycle(abts_case *tc, void *data)
{
    ogs_socknode_t *ngap = testngap_client(1, AF_INET);
    ogs_socknode_t *gtpu = test_gtpu_server(1, AF_INET);
    test_ue_t *ue = register_ue(tc, ngap);
    test_sess_t *sess = establish_session(tc, ue, ngap, 5);
    test_bearer_t *flow;
    char app_id[256], path[258];
    cJSON *body, *qos;
    const char *invalid[] = {"0", "128", "-1", "20.5", "null", "true", "\"20\"", "{}", "[]", "1e300"};
    unsigned int i, cycles = 1;
    const char *cycle_env = getenv("XCN_QOS_CYCLES");
    if (cycle_env) cycles = strtoul(cycle_env, NULL, 10);
    ogs_assert(cycles >= 1 && cycles <= 10000);
    ping_flow(tc, gtpu, test_qos_flow_find_by_qfi(sess, 1));

    /* Omitted priority on creation must not be replaced by the ARP value. */
    body = bearer_body(sess, 0);
    cJSON_Delete(request(tc, "POST", "", body, 201, app_id));
    modify_complete(tc, sess, ngap, 0, false);
    query_bearer(tc, sess, app_id, 0, 1);
    ogs_snprintf(path, sizeof(path), "/%s", app_id);
    cJSON_Delete(request(tc, "DELETE", path, NULL, 204, NULL));
    modify_complete(tc, sess, ngap, 0, true);
    cJSON_Delete(body);

    body = bearer_body(sess, 20);
    qos = cJSON_GetObjectItemCaseSensitive(body, "qos");
    for (i = 0; i < OGS_ARRAY_SIZE(invalid); i++) {
        cJSON_ReplaceItemInObjectCaseSensitive(qos, "priorityLevel", cJSON_Parse(invalid[i]));
        cJSON_Delete(request(tc, "POST", "", body, 400, NULL));
    }
    query_bearer(tc, sess, NULL, 0, 0);
    cJSON_ReplaceItemInObjectCaseSensitive(qos, "priorityLevel", cJSON_CreateNumber(20));
    cJSON_Delete(request(tc, "POST", "", body, 201, app_id));
    modify_complete(tc, sess, ngap, 20, false);
    ogs_snprintf(path, sizeof(path), "/%s", app_id);
    query_bearer(tc, sess, app_id, 20, 1);
    ping_flow(tc, gtpu, dedicated_flow(sess));
    recover_access(tc, sess, ngap, 20);
    for (i = 0; i < OGS_ARRAY_SIZE(invalid); i++) {
        cJSON_ReplaceItemInObjectCaseSensitive(qos, "priorityLevel", cJSON_Parse(invalid[i]));
        cJSON_Delete(request(tc, "PATCH", path, body, 400, NULL));
        query_bearer(tc, sess, app_id, 20, 1);
    }
    cJSON_Delete(body);
    cJSON_Delete(request(tc, "DELETE", path, NULL, 204, NULL));
    modify_complete(tc, sess, ngap, 0, true);
    query_bearer(tc, sess, NULL, 0, 0);

    for (i = 0; i < cycles; i++) {
        int priority = i % 2 ? 127 : 1;
        body = bearer_body(sess, priority);
        cJSON_Delete(request(tc, "POST", "", body, 201, app_id));
        flow = modify_complete(tc, sess, ngap, priority, false);
        ogs_snprintf(path, sizeof(path), "/%s", app_id);
        query_bearer(tc, sess, app_id, priority, 1);
        qos = cJSON_GetObjectItemCaseSensitive(body, "qos");
        cJSON_ReplaceItemInObjectCaseSensitive(qos, "priorityLevel", cJSON_CreateNumber(30));
        cJSON_Delete(request(tc, "PATCH", path, body, 200, NULL));
        ABTS_PTR_EQUAL(tc, flow, modify_complete(tc, sess, ngap, 30, false));
        query_bearer(tc, sess, app_id, 30, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(qos, "priorityLevel");
        cJSON_Delete(request(tc, "PATCH", path, body, 200, NULL));
        ABTS_PTR_EQUAL(tc, flow, modify_complete(tc, sess, ngap, 0, false));
        query_bearer(tc, sess, app_id, 0, 1);
        if (i == 0) recover_access(tc, sess, ngap, 0);
        ping_flow(tc, gtpu, flow);
        cJSON_Delete(request(tc, "DELETE", path, NULL, 204, NULL));
        modify_complete(tc, sess, ngap, 0, true);
        query_bearer(tc, sess, NULL, 0, 0);
        ABTS_INT_EQUAL(tc, 1, ogs_list_count(&sess->bearer_list));
        ping_flow(tc, gtpu, test_qos_flow_find_by_qfi(sess, 1));
        cJSON_Delete(body);
        if (!((i + 1) % 25))
            fprintf(stderr, "QoS priority: %u complete lifecycles\n", i + 1);
    }
    release_session(tc, sess, ngap);
    send_ngap(tc, ngap, testngap_build_ue_context_release_request(ue,
                NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity, true));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_UEContextRelease);
    send_ngap(tc, ngap, testngap_build_ue_context_release_complete(ue));
    ogs_msleep(100);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(ue));
    testgnb_gtpu_close(gtpu);
    testgnb_ngap_close(ngap);
    test_ue_remove(ue);
}

typedef struct {
    const char *method;
    const char *path;
    cJSON *body;
    int expected;
    char *app_id;
} parallel_request_t;

static void *parallel_request(void *arg)
{
    parallel_request_t *r = arg;
    /* ABTS progress counters are shared and must stay on the main thread. */
    cJSON_Delete(request(NULL, r->method, r->path,
                r->body, r->expected, r->app_id));
    return NULL;
}

static void concurrent_lifecycle(abts_case *tc, void *data)
{
    ogs_socknode_t *ngap = testngap_client(1, AF_INET);
    ogs_socknode_t *gtpu = test_gtpu_server(1, AF_INET);
    test_ue_t *ue = register_ue(tc, ngap);
    test_sess_t *sessions[2];
    char app_ids[2][256], paths[2][258];
    cJSON *bodies[2];
    unsigned int cycle, cycles = 1;
    int j, stage;
    const char *cycle_env = getenv("XCN_QOS_CYCLES");
    if (cycle_env) cycles = strtoul(cycle_env, NULL, 10);
    ogs_assert(cycles >= 1 && cycles <= 10000);
    sessions[0] = establish_session(tc, ue, ngap, 5);
    sessions[1] = establish_session(tc, ue, ngap, 6);
    for (cycle = 0; cycle < cycles; cycle++) {
        bodies[0] = bearer_body(sessions[0], 1);
        bodies[1] = bearer_body(sessions[1], 127);
        for (stage = 0; stage < 4; stage++) {
            pthread_t threads[2];
            parallel_request_t requests[2];
            for (j = 0; j < 2; j++) {
                cJSON *qos = cJSON_GetObjectItemCaseSensitive(bodies[j], "qos");
                if (stage == 1)
                    cJSON_ReplaceItemInObjectCaseSensitive(qos, "priorityLevel",
                            cJSON_CreateNumber(j ? 50 : 20));
                if (stage == 2)
                    cJSON_DeleteItemFromObjectCaseSensitive(qos, "priorityLevel");
                requests[j].method = stage == 0 ? "POST" : stage == 3 ? "DELETE" : "PATCH";
                requests[j].path = stage == 0 ? "" : paths[j];
                requests[j].body = stage == 3 ? NULL : bodies[j];
                requests[j].expected = stage == 0 ? 201 : stage == 3 ? 204 : 200;
                requests[j].app_id = stage == 0 ? app_ids[j] : NULL;
                ogs_assert(pthread_create(&threads[j], NULL, parallel_request, &requests[j]) == 0);
            }
            for (j = 0; j < 2; j++) {
                ogs_assert(pthread_join(threads[j], NULL) == 0);
                ogs_snprintf(paths[j], sizeof(paths[j]), "/%s", app_ids[j]);
            }
            /* Notifications may arrive in either session order. */
            for (j = 0; j < 2; j++) {
                int psi = receive_ngap(tc, ue, ngap,
                        NGAP_ProcedureCode_id_PDUSessionResourceModify);
                int idx = psi == 5 ? 0 : 1;
                int priority = stage == 0 ? (idx ? 127 : 1) :
                    stage == 1 ? (idx ? 50 : 20) : 0;
                ogs_assert(psi == 5 || psi == 6);
                finish_modify(tc, sessions[idx], ngap, priority, stage == 3);
            }
            for (j = 0; j < 2; j++) {
                int priority = stage == 0 ? (j ? 127 : 1) :
                    stage == 1 ? (j ? 50 : 20) : 0;
                query_bearer(tc, sessions[j], stage == 3 ? NULL : app_ids[j],
                        priority, stage == 3 ? 0 : 1);
                ping_flow(tc, gtpu, stage == 3 ?
                        test_qos_flow_find_by_qfi(sessions[j], 1) : dedicated_flow(sessions[j]));
            }
        }
        for (j = 0; j < 2; j++) {
            ABTS_INT_EQUAL(tc, 1, ogs_list_count(&sessions[j]->bearer_list));
            cJSON_Delete(bodies[j]);
        }
    }
    release_session(tc, sessions[1], ngap);
    release_session(tc, sessions[0], ngap);
    send_ngap(tc, ngap, testngap_build_ue_context_release_request(ue,
                NGAP_Cause_PR_radioNetwork, NGAP_CauseRadioNetwork_user_inactivity, true));
    receive_ngap(tc, ue, ngap, NGAP_ProcedureCode_id_UEContextRelease);
    send_ngap(tc, ngap, testngap_build_ue_context_release_complete(ue));
    ogs_msleep(100);
    ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(ue));
    testgnb_gtpu_close(gtpu);
    testgnb_ngap_close(ngap);
    test_ue_remove(ue);
    fprintf(stderr, "QoS priority: %u concurrent two-session lifecycles complete\n", cycles);
}

abts_suite *test_dedicated_bearer(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, priority_lifecycle, NULL);
    abts_run_test(suite, concurrent_lifecycle, NULL);
    return suite;
}
