/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/amf/context.h"
#include "../../src/amf/ue-info.h"
#include "../../src/amf/gnb-info.h"
#include "../../src/amf/ue-events.h"

static const char *event_arg(void *request, const char *key)
{
    return !strcmp(key, "detail") ? "1" : NULL;
}

static void test_event_metadata(ogs_sock_t *sock);

static const cJSON *get(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static cJSON *export_ue(void)
{
    char buf[16384];
    cJSON *root, *ue;
    ogs_assert(amf_dump_ue_info(buf, sizeof(buf), 0, 100) > 0);
    root = cJSON_Parse(buf);
    ogs_assert(root);
    ue = cJSON_Duplicate(cJSON_GetArrayItem(get(root, "items"), 0), true);
    ogs_assert(ue);
    cJSON_Delete(root);
    return ue;
}

static amf_gnb_t *make_gnb(ogs_sock_t *sock, int mnc)
{
    ogs_sockaddr_t *addr = ogs_calloc(1, sizeof(*addr));
    amf_gnb_t *gnb;
    ogs_assert(addr);
    addr->ogs_sa_family = AF_INET;
    addr->sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin.sin_port = htons(38412 + mnc);
    gnb = amf_gnb_add(sock, addr);
    ogs_assert(gnb);
    gnb->max_num_of_ostreams = 2;
    ogs_plmn_id_build(&gnb->plmn_id, 1, mnc, 2);
    return gnb;
}

static void test_width(ogs_sock_t *sock, unsigned bits)
{
    amf_gnb_t *gnb = make_gnb(sock, 1), *target;
    ran_ue_t *ran;
    amf_ue_t ue;
    ogs_guami_t guami;
    cJSON *out;
    const cJSON *loc, *cgi;
    NGAP_GlobalGNB_ID_t global;
    uint32_t id = bits == 32 ? UINT32_MAX : 100;
    uint32_t decoded = 0;
    uint8_t decoded_bits = 0;
    uint64_t cell = (1ULL << (36 - bits)) - 1;
    char buf[16384];
    char supi[] = "imsi-001010000000001";

    memset(&global, 0, sizeof(global));
    ogs_ngap_uint32_to_GNB_ID(id, bits, &global.gNB_ID);
    ogs_asn_buffer_to_OCTET_STRING(&gnb->plmn_id, OGS_PLMN_ID_LEN,
            &global.pLMNIdentity);
    ogs_assert(amf_gnb_decode_global_id(&global, &decoded, &decoded_bits));
    ogs_assert(decoded == id && decoded_bits == bits);
    global.gNB_ID.choice.gNB_ID.size = 2;
    ogs_assert(!amf_gnb_decode_global_id(&global, &decoded, &decoded_bits));
    global.gNB_ID.choice.gNB_ID.size = (bits + 7) / 8;
    global.gNB_ID.choice.gNB_ID.bits_unused = 8;
    ogs_assert(!amf_gnb_decode_global_id(&global, &decoded, &decoded_bits));
    global.gNB_ID.choice.gNB_ID.bits_unused = (8 - bits % 8) % 8;
    if (global.gNB_ID.choice.gNB_ID.bits_unused) {
        global.gNB_ID.choice.gNB_ID.buf[global.gNB_ID.choice.gNB_ID.size - 1] |= 1;
        ogs_assert(!amf_gnb_decode_global_id(&global, &decoded, &decoded_bits));
    }
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_GlobalGNB_ID, &global);
    ogs_assert(amf_gnb_set_gnb_id(gnb, id, bits) == OGS_OK);
    ogs_assert(amf_gnb_set_gnb_id(gnb, id, 21) == OGS_ERROR);
    ogs_assert(amf_gnb_set_gnb_id(gnb, id, 33) == OGS_ERROR);
    ogs_assert(gnb->gnb_id_bits == bits);

    ran = ran_ue_add(gnb, 7);
    ogs_assert(ran);
    memset(&ue, 0, sizeof(ue));
    memset(&guami, 0, sizeof(guami));
    ue.guami = &guami;
    ue.supi = supi;
    ogs_list_init(&ue.sess_list);
    ogs_list_add(&amf_self()->amf_ue_list, &ue);
    amf_ue_associate_ran_ue(&ue, ran);
    ran->saved.nr_cgi.plmn_id = gnb->plmn_id;
    ran->saved.nr_tai.plmn_id = gnb->plmn_id;
    ran->saved.nr_tai.tac.v = 1;
    ran->saved.nr_cgi.cell_id = ((uint64_t)id << (36 - bits)) | cell;
    ue.ue_location_timestamp = 1;
    amf_ue_update_location(&ue, ran);
    ogs_assert(ue.ue_location_timestamp > 1);
    out = export_ue();
    loc = get(out, "location");
    cgi = get(loc, "nr_cgi");
    ogs_assert(!strcmp(get(loc, "status")->valuestring, "reported"));
    ogs_assert((uint64_t)get(get(out, "gnb"), "gnb_id")->valuedouble == id);
    ogs_assert((uint64_t)get(get(out, "gnb"), "cell_id")->valuedouble == cell);
    ogs_assert(get(cgi, "gnb_id_bits")->valueint == bits);
    ogs_assert(cJSON_IsTrue(get(cgi, "identity_consistent")));
    ogs_assert((uint64_t)get(cgi, "cell_id")->valuedouble == cell);
    ogs_assert((uint64_t)get(cgi, "nci")->valuedouble == ran->saved.nr_cgi.cell_id);
    cJSON_Delete(out);

    /* A new gNB with the same numeric ID must be resolved by RAN association. */
    target = make_gnb(sock, 2);
    ogs_assert(amf_gnb_set_gnb_id(target, id, bits) == OGS_OK);
    ran_ue_switch_to_gnb(ran, target);
    ran->saved.nr_cgi.plmn_id = target->plmn_id;
    amf_ue_update_location(&ue, ran);
    out = export_ue();
    ogs_assert(!strcmp(get(get(out, "gnb"), "plmn")->valuestring, "00102"));
    cJSON_Delete(out);
    ogs_assert(amf_dump_gnb_info(buf, sizeof(buf), 0, 100) > 0);
    out = cJSON_Parse(buf);
    ogs_assert(cJSON_GetArraySize(get(out, "items")) == 2);
    cJSON_Delete(out);

    /* Configuration updates do not rewrite the historical location snapshot. */
    ogs_assert(amf_gnb_set_gnb_id(target, 101, 24) == OGS_OK);
    ogs_assert(ue.location_gnb.id == id && ue.location_gnb.bits == bits);
    ran->saved.nr_cgi.cell_id = (101ULL << 12) | 3;
    ue.ue_location_timestamp = 1;
    amf_ue_update_location(&ue, ran);
    ogs_assert(ue.location_gnb.id == 101 && ue.location_gnb.bits == 24);
    ogs_assert(ue.ue_location_timestamp > 1);

    ran->saved.nr_cgi.cell_id = (102ULL << 12) | 3;
    amf_ue_update_location(&ue, ran);
    out = export_ue();
    cgi = get(get(out, "location"), "nr_cgi");
    ogs_assert(cJSON_IsFalse(get(cgi, "identity_consistent")));
    ogs_assert(get(cgi, "gnb_id")->valueint == 102);
    ogs_assert(get(cgi, "cell_id")->valueint == 3);
    ogs_assert(get(get(out, "gnb"), "gnb_id")->valueint == 101);
    ogs_assert(get(get(out, "gnb"), "cell_id")->valueint == 3);
    cJSON_Delete(out);

    ue.nr_cgi.cell_id = 1ULL << 36;
    out = export_ue();
    cgi = get(get(out, "location"), "nr_cgi");
    ogs_assert(cJSON_IsNull(get(cgi, "nci")));
    ogs_assert(cJSON_IsNull(get(cgi, "nci_hex")));
    ogs_assert(cJSON_IsNull(get(cgi, "gnb_id")));
    ogs_assert(cJSON_IsNull(get(cgi, "cell_id")));
    ogs_assert(cJSON_IsNull(get(cgi, "identity_consistent")));
    ogs_assert(cJSON_IsNull(get(get(out, "gnb"), "cell_id")));
    cJSON_Delete(out);
    ue.nr_cgi.cell_id = (102ULL << 12) | 3;

    ue.location_gnb.bits = 21;
    out = export_ue();
    cgi = get(get(out, "location"), "nr_cgi");
    ogs_assert(cJSON_IsNull(get(cgi, "gnb_id_bits")));
    ogs_assert(cJSON_IsNull(get(cgi, "cell_id")));
    ogs_assert(cJSON_IsNull(get(cgi, "identity_consistent")));
    cJSON_Delete(out);
    ue.location_gnb.bits = 33;
    out = export_ue();
    ogs_assert(cJSON_IsNull(get(get(get(out, "location"), "nr_cgi"), "cell_id")));
    cJSON_Delete(out);
    ue.location_gnb.bits = 24;

    amf_ue_deassociate_ran_ue(&ue, ran);
    ran_ue_remove(ran);
    amf_gnb_remove(gnb);
    amf_gnb_remove(target);
    out = export_ue();
    loc = get(out, "location");
    ogs_assert(!strcmp(get(loc, "status")->valuestring, "last-known"));
    ogs_assert(get(get(loc, "gnb"), "gnb_id")->valueint == 101);
    ogs_assert(strstr(get(get(loc, "gnb"), "peer")->valuestring, "127.0.0.1"));
    cgi = get(loc, "nr_cgi");
    ogs_assert(get(cgi, "gnb_id")->valueint == 102);
    ogs_assert(get(cgi, "cell_id")->valueint == 3);
    ogs_assert(cJSON_IsFalse(get(cgi, "identity_consistent")));
    cJSON_Delete(out);
    ue.ue_location_timestamp = 0;
    out = export_ue();
    ogs_assert(!strcmp(get(get(out, "location"), "status")->valuestring, "unknown"));
    ogs_assert(cJSON_IsNull(get(get(get(out, "location"), "nr_cgi"), "cell_id")));
    cJSON_Delete(out);
    ogs_list_remove(&amf_self()->amf_ue_list, &ue);
}


static void test_event_metadata(ogs_sock_t *sock)
{
    amf_gnb_t *source = make_gnb(sock, 1), *target = make_gnb(sock, 2);
    ogs_assert(ogs_ue_events_init("AMF", 64, 32));
    ogs_assert(amf_gnb_set_gnb_id(source, 100, 28) == OGS_OK);
    ogs_assert(amf_gnb_set_gnb_id(target, 200, 28) == OGS_OK);
    ran_ue_t *ran = ran_ue_add(source, 7);
    ogs_assert(ran);
    ogs_ngap_message_t pdu = {0};
    pdu.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage = ogs_calloc(1, sizeof(*pdu.choice.initiatingMessage));
    pdu.choice.initiatingMessage->procedureCode = NGAP_ProcedureCode_id_PathSwitchRequest;
    pdu.choice.initiatingMessage->value.present = NGAP_InitiatingMessage__value_PR_PathSwitchRequest;
    NGAP_PathSwitchRequest_t *notify = &pdu.choice.initiatingMessage->value.choice.PathSwitchRequest;
    NGAP_PathSwitchRequestIEs_t *amf = ogs_calloc(1, sizeof(*amf));
    amf->id = NGAP_ProtocolIE_ID_id_SourceAMF_UE_NGAP_ID;
    amf->value.present = NGAP_PathSwitchRequestIEs__value_PR_AMF_UE_NGAP_ID;
    asn_uint642INTEGER(&amf->value.choice.AMF_UE_NGAP_ID, ran->amf_ue_ngap_id);
    ASN_SEQUENCE_ADD(&notify->protocolIEs.list, amf);
    NGAP_PathSwitchRequestIEs_t *remote = ogs_calloc(1, sizeof(*remote));
    remote->id = NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
    remote->value.present = NGAP_PathSwitchRequestIEs__value_PR_RAN_UE_NGAP_ID;
    remote->value.choice.RAN_UE_NGAP_ID = 99;
    ASN_SEQUENCE_ADD(&notify->protocolIEs.list, remote);
    NGAP_PathSwitchRequestIEs_t *location = ogs_calloc(1, sizeof(*location));
    location->id = NGAP_ProtocolIE_ID_id_UserLocationInformation;
    location->value.present = NGAP_PathSwitchRequestIEs__value_PR_UserLocationInformation;
    NGAP_UserLocationInformation_t *uli = &location->value.choice.UserLocationInformation;
    uli->present = NGAP_UserLocationInformation_PR_userLocationInformationNR;
    uli->choice.userLocationInformationNR = ogs_calloc(1, sizeof(*uli->choice.userLocationInformationNR));
    ogs_nr_cgi_t cgi = {.cell_id = (200ULL << 8) | 78};
    ogs_5gs_tai_t tai = {0}; tai.tac.v = 1;
    ogs_plmn_id_build(&cgi.plmn_id, 1, 2, 2); tai.plmn_id = cgi.plmn_id;
    ogs_ngap_nr_cgi_to_ASN(&cgi, &uli->choice.userLocationInformationNR->nR_CGI);
    ogs_ngap_5gs_tai_to_ASN(&tai, &uli->choice.userLocationInformationNR->tAI);
    ASN_SEQUENCE_ADD(&notify->protocolIEs.list, location);
    amf_ue_event_ngap(target, &pdu, "gNB->AMF", "received");
    ogs_ue_events_bind(ran->event_trace_id, "imsi-001010000000001");
    int status = 0;
    char *text = ogs_ue_events_query(event_arg, NULL, &status);
    ogs_assert(status == 200 && text);
    cJSON *root = cJSON_Parse(text); free(text);
    const cJSON *event = cJSON_GetArrayItem(get(root, "items"), 0);
    ogs_assert(get(event, "gnb_id")->valueint == 200);
    ogs_assert(!strcmp(get(event, "ran_ngap_id")->valuestring, "99"));
    ogs_assert(get(event, "cell_id")->valueint == 78);
    ogs_assert(!strcmp(get(event, "cell_plmn")->valuestring, "00102"));
    cJSON_Delete(root);
    /* A 40-bit wire ID must not wrap onto a 32-bit pool slot. */
    asn_uint642INTEGER(&amf->value.choice.AMF_UE_NGAP_ID, (1ULL << 32) | ran->amf_ue_ngap_id);
    amf_ue_event_ngap(target, &pdu, "gNB->AMF", "received");
    text = ogs_ue_events_query(event_arg, NULL, &status);
    root = cJSON_Parse(text); free(text);
    event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
    ogs_assert(!get(event, "supi")->valuestring[0]);
    cJSON_Delete(root);
    asn_uint642INTEGER(&amf->value.choice.AMF_UE_NGAP_ID, ran->amf_ue_ngap_id);
    /* Trace extraction runs before protocol validation: malformed IEs must not assert. */
    uli->choice.userLocationInformationNR->nR_CGI.nRCellIdentity.size = 2;
    amf_ue_event_ngap(target, &pdu, "gNB->AMF", "received");
    text = ogs_ue_events_query(event_arg, NULL, &status);
    root = cJSON_Parse(text); free(text);
    event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
    ogs_assert(cJSON_IsNull(get(event, "nci")));
    ogs_assert(!strcmp(get(event, "supi")->valuestring, "imsi-001010000000001"));
    cJSON_Delete(root);
    /* Metadata decoded before a consuming send owns its ASN.1 buffers. */
    uli->choice.userLocationInformationNR->nR_CGI.nRCellIdentity.size = 5;
    ogs_pkbuf_t *encoded = ogs_ngap_encode(&pdu);
    ogs_assert(encoded);
    ogs_ngap_message_t decoded = {0};
    ogs_assert(ogs_ngap_decode(&decoded, encoded) == OGS_OK);
    ogs_pkbuf_free(encoded);
    amf_ue_event_ngap(target, &decoded, "AMF->gNB", "queued");
    NGAP_PathSwitchRequestIEs_t *zero_id = decoded.choice.initiatingMessage->value.choice.PathSwitchRequest.protocolIEs.list.array[0];
    asn_uint642INTEGER(&zero_id->value.choice.AMF_UE_NGAP_ID, 0);
    amf_ue_event_ngap(target, &decoded, "gNB->AMF", "received");
    text = ogs_ue_events_query(event_arg, NULL, &status);
    root = cJSON_Parse(text); free(text);
    event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
    ogs_assert(strstr(get(event, "info")->valuestring, "AMF-UE-ID=0 "));
    cJSON_Delete(root);
    ogs_ngap_free(&decoded);
    /* InitialUEMessage has no AMF UE ID; a present wire ID of zero is valid. */
    ogs_ngap_message_t initial = {0};
    initial.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    initial.choice.initiatingMessage = ogs_calloc(1, sizeof(*initial.choice.initiatingMessage));
    initial.choice.initiatingMessage->value.present = NGAP_InitiatingMessage__value_PR_InitialUEMessage;
    NGAP_InitialUEMessage_IEs_t *initial_ran = ogs_calloc(1, sizeof(*initial_ran));
    initial_ran->value.present = NGAP_InitialUEMessage_IEs__value_PR_RAN_UE_NGAP_ID;
    initial_ran->value.choice.RAN_UE_NGAP_ID = 7;
    ASN_SEQUENCE_ADD(&initial.choice.initiatingMessage->value.choice.InitialUEMessage.protocolIEs.list, initial_ran);
    amf_ue_event_ngap(source, &initial, "gNB->AMF", "received");
    text = ogs_ue_events_query(event_arg, NULL, &status);
    root = cJSON_Parse(text); free(text);
    event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
    ogs_assert(!strcmp(get(event, "name")->valuestring, "InitialUEMessage"));
    ogs_assert(!strstr(get(event, "info")->valuestring, "AMF-UE-ID="));
    ogs_assert(strstr(get(event, "info")->valuestring, "RAN-UE-ID=7"));
    cJSON_Delete(root);
    ogs_ngap_free(&initial);
    /* NAS transport carries one Session ID; UE-wide messages must stay unassigned. */
    for (unsigned i = 0; i < 4; i++) {
        ogs_nas_5gs_message_t nas = {0};
        if (i == 1) {
            nas.gmm.h.message_type = OGS_NAS_5GS_DL_NAS_TRANSPORT;
            nas.gmm.dl_nas_transport.presencemask = OGS_NAS_5GS_DL_NAS_TRANSPORT_PDU_SESSION_ID_PRESENT;
            nas.gmm.dl_nas_transport.pdu_session_id = 7;
        } else {
            nas.gmm.h.message_type = OGS_NAS_5GS_UL_NAS_TRANSPORT;
            if (i != 2) nas.gmm.ul_nas_transport.presencemask = OGS_NAS_5GS_UL_NAS_TRANSPORT_PDU_SESSION_ID_PRESENT;
            nas.gmm.ul_nas_transport.pdu_session_id = i == 3 ? 16 : 5;
        }
        amf_ue_event_nas(ran, NULL, &nas, i != 1);
        text = ogs_ue_events_query(event_arg, NULL, &status);
        ogs_assert(text && status == 200);
        root = cJSON_Parse(text); free(text);
        event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
        if (i < 2) ogs_assert(get(event, "psi")->valueint == (i ? 7 : 5));
        else ogs_assert(cJSON_IsNull(get(event, "psi")));
        cJSON_Delete(root);
    }
    /* RX metadata uses the response reference without changing Session state. */
    struct {
        const char *location, *cached, *expected, *name;
        int status;
    } responses[] = {
        {"http://127.0.0.4:7777/nsmf-pdusession/v1/sm-contexts/42", NULL, "42", "CreateSMContextResponse", 201},
        {"http://127.0.0.4:7777/nsmf-pdusession/v1/sm-contexts/ctx-%37?ignored=1", "old", "ctx-7", "CreateSMContextResponse", 201},
        {NULL, "old", "missing-location", "CreateSMContextResponse", 201},
        {"", "old", "missing-location", "CreateSMContextResponse", 201},
        {"http://127.0.0.4:7777", "old", "invalid-location", "CreateSMContextResponse", 201},
        {"http://127.0.0.4:7777/nsmf-pdusession", "old", "invalid-location", "CreateSMContextResponse", 201},
        {"http://127.0.0.4:7777/nsmf-pdusession/v1/sm-contexts", "old", "invalid-location", "CreateSMContextResponse", 201},
        {"not-a-uri", "old", "invalid-location", "CreateSMContextResponse", 201},
        {NULL, "current", "current", "UpdateSMContextResponse", 204},
        {NULL, NULL, "-", "CreateSMContextResponse", 500}
    };
    char supi[] = "imsi-001010000000001", dnn[] = "internet";
    amf_ue_t ue = {.supi = supi};
    amf_sess_t sess = {.psi = 2, .dnn = dnn};
    for (unsigned i = 0; i < OGS_ARRAY_SIZE(responses); i++) {
        ogs_sbi_message_t response = {0};
        char location[256] = "";
        if (responses[i].location) {
            snprintf(location, sizeof(location), "%s", responses[i].location);
            response.http.location = location;
        }
        response.res_status = responses[i].status;
        sess.sm_context_ref = (char *)responses[i].cached;
        amf_ue_event_session(&ue, ran, &sess, &response, responses[i].name);
        ogs_assert(sess.sm_context_ref == responses[i].cached);
        ogs_assert(!sess.sm_context_resource_uri);
        if (responses[i].location) ogs_assert(!strcmp(location, responses[i].location));
        memset(location, 'x', sizeof(location));
        text = ogs_ue_events_query(event_arg, NULL, &status);
        ogs_assert(text && status == 200);
        root = cJSON_Parse(text); free(text);
        event = cJSON_GetArrayItem(get(root, "items"), cJSON_GetArraySize(get(root, "items"))-1);
        const char *context = strstr(get(event, "info")->valuestring, "SM-context=");
        ogs_assert(context && !strcmp(context + strlen("SM-context="), responses[i].expected));
        ogs_assert(!strcmp(get(event, "result")->valuestring, responses[i].status >= 400 ? "failed" : "received"));
        cJSON_Delete(root);
    }
    ran_ue_remove(ran); amf_gnb_remove(source); amf_gnb_remove(target);
    ogs_ue_events_final();
}

int main(void)
{
    unsigned i;
    const unsigned widths[] = {22, 23, 24, 28, 32};
    ogs_sock_t sock;
    ogs_socknode_t node;
    ogs_pkbuf_config_t packet_config;

    ogs_core_initialize();
    ogs_log_install_domain(&__ogs_sbi_domain, "sbi", OGS_LOG_ERROR);
    ogs_pkbuf_default_init(&packet_config);
    ogs_pkbuf_default_create(&packet_config);
    ogs_app_context_init();
    ogs_app_config_init();
    ogs_global_conf()->max.peer = 8;
    ogs_global_conf()->max.ue = 16;
    ogs_app()->pool.sess = 32;
    ogs_app()->metrics.max_specs = 256;
    ogs_app()->timer_mgr = ogs_timer_mgr_create(64);
    amf_metrics_init();
    amf_context_init();
    memset(&sock, 0, sizeof(sock));
    memset(&node, 0, sizeof(node));
    node.sock = &sock;
    ogs_list_add(&amf_self()->ngap_list, &node);
    for (i = 0; i < OGS_ARRAY_SIZE(widths); i++) test_width(&sock, widths[i]);
    test_event_metadata(&sock);
    ogs_list_remove(&amf_self()->ngap_list, &node);
    amf_context_final();
    amf_metrics_final();
    ogs_app_config_final();
    ogs_app_context_final();
    ogs_pkbuf_default_destroy();
    ogs_core_terminate();
    /* Core teardown frees its pool; release talloc's process-wide debug root. */
    talloc_disable_null_tracking();
    puts("AMF location: all checks passed");
    return 0;
}
