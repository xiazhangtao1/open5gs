/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/amf/ngap-build.h"
#include "node-profile-fixture.h"

static void event_transfer_identity(NGAP_GlobalRANNodeID_t *id, NGAP_TAI_t *tai,
        unsigned value)
{
    ogs_5gs_tai_t location = {0};
    ogs_plmn_id_build(&location.plmn_id, 460, 11, 2); location.tac.v = 1;
    id->present = NGAP_GlobalRANNodeID_PR_globalGNB_ID;
    id->choice.globalGNB_ID = ogs_calloc(1, sizeof(*id->choice.globalGNB_ID));
    ogs_asn_buffer_to_OCTET_STRING(&location.plmn_id, OGS_PLMN_ID_LEN,
            &id->choice.globalGNB_ID->pLMNIdentity);
    ogs_ngap_uint32_to_GNB_ID(value, 22, &id->choice.globalGNB_ID->gNB_ID);
    ogs_ngap_5gs_tai_to_ASN(&location, tai);
}

/* Valid PER fixtures use production builders where available. */
static ogs_pkbuf_t *event_fixture(unsigned index, amf_ue_t *ue)
{
    ogs_ngap_message_t pdu = {0};
    if (index == 0) {
        node_profile_setup_fixture(&pdu, "signalling-fixture", 18, 1, 3584);
        return ogs_ngap_encode(&pdu);
    }
    if (index == 1) return ngap_build_ng_setup_response();
    if (index == 2) return ngap_build_ng_setup_failure(NGAP_Cause_PR_misc, NGAP_CauseMisc_unspecified, NGAP_TimeToWait_v10s);
    if (index == 4) return ogs_ngap_build_ng_reset_ack(NULL);
    if (index == 6) return ngap_build_ran_configuration_update_ack();
    if (index == 7) return ngap_build_ran_configuration_update_failure(NGAP_Cause_PR_misc, NGAP_CauseMisc_unspecified, NGAP_TimeToWait_v10s);
    if (index == 8) return ngap_build_amf_configuration_update();
    if (index == 9) return ngap_build_paging(ue);
    pdu.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    NGAP_InitiatingMessage_t *p = pdu.choice.initiatingMessage = ogs_calloc(1, sizeof(*p));
    p->criticality = NGAP_Criticality_ignore;
    if (index == 3) {
        p->procedureCode = NGAP_ProcedureCode_id_NGReset;
        p->value.present = NGAP_InitiatingMessage__value_PR_NGReset;
        NGAP_NGResetIEs_t *ie = ogs_calloc(1, sizeof(*ie));
        ie->id = NGAP_ProtocolIE_ID_id_Cause;
        ie->criticality = NGAP_Criticality_ignore;
        ie->value.present = NGAP_NGResetIEs__value_PR_Cause;
        ie->value.choice.Cause.present = NGAP_Cause_PR_misc;
        ie->value.choice.Cause.choice.misc = NGAP_CauseMisc_unspecified;
        ASN_SEQUENCE_ADD(&p->value.choice.NGReset.protocolIEs, ie);
        ie = ogs_calloc(1, sizeof(*ie));
        ie->id = NGAP_ProtocolIE_ID_id_ResetType;
        ie->criticality = NGAP_Criticality_reject;
        ie->value.present = NGAP_NGResetIEs__value_PR_ResetType;
        ie->value.choice.ResetType.present = NGAP_ResetType_PR_nG_Interface;
        ie->value.choice.ResetType.choice.nG_Interface = NGAP_ResetAll_reset_all;
        ASN_SEQUENCE_ADD(&p->value.choice.NGReset.protocolIEs, ie);
    } else if (index == 5) {
        p->procedureCode = NGAP_ProcedureCode_id_RANConfigurationUpdate;
        p->value.present = NGAP_InitiatingMessage__value_PR_RANConfigurationUpdate;
    } else if (index == 10 || index == 11) {
        NGAP_SONConfigurationTransfer_t *transfer;
        if (index == 10) {
            p->procedureCode = NGAP_ProcedureCode_id_UplinkRANConfigurationTransfer;
            p->value.present = NGAP_InitiatingMessage__value_PR_UplinkRANConfigurationTransfer;
            NGAP_UplinkRANConfigurationTransferIEs_t *ie = ogs_calloc(1, sizeof(*ie));
            ie->id = NGAP_ProtocolIE_ID_id_SONConfigurationTransferUL;
            ie->criticality = NGAP_Criticality_ignore;
            ie->value.present = NGAP_UplinkRANConfigurationTransferIEs__value_PR_SONConfigurationTransfer;
            transfer = &ie->value.choice.SONConfigurationTransfer;
            ASN_SEQUENCE_ADD(&p->value.choice.UplinkRANConfigurationTransfer.protocolIEs, ie);
        } else {
            p->procedureCode = NGAP_ProcedureCode_id_DownlinkRANConfigurationTransfer;
            p->value.present = NGAP_InitiatingMessage__value_PR_DownlinkRANConfigurationTransfer;
            NGAP_DownlinkRANConfigurationTransferIEs_t *ie = ogs_calloc(1, sizeof(*ie));
            ie->id = NGAP_ProtocolIE_ID_id_SONConfigurationTransferDL;
            ie->criticality = NGAP_Criticality_ignore;
            ie->value.present = NGAP_DownlinkRANConfigurationTransferIEs__value_PR_SONConfigurationTransfer;
            transfer = &ie->value.choice.SONConfigurationTransfer;
            ASN_SEQUENCE_ADD(&p->value.choice.DownlinkRANConfigurationTransfer.protocolIEs, ie);
        }
        event_transfer_identity(&transfer->sourceRANNodeID.globalRANNodeID, &transfer->sourceRANNodeID.selectedTAI, 3584);
        event_transfer_identity(&transfer->targetRANNodeID.globalRANNodeID, &transfer->targetRANNodeID.selectedTAI, 3585);
        transfer->sONInformation.present = NGAP_SONInformation_PR_sONInformationRequest;
        transfer->sONInformation.choice.sONInformationRequest = NGAP_SONInformationRequest_xn_TNL_configuration_info;
    } else {
        p->procedureCode = NGAP_ProcedureCode_id_OverloadStop;
        p->value.present = NGAP_InitiatingMessage__value_PR_OverloadStop;
    }
    return ogs_ngap_encode(&pdu);
}

static void test_base_station_events(ogs_sock_t *sock)
{
    static const char *names[] = {"NGSetupRequest", "NGSetupResponse", "NGSetupFailure", "NGReset",
        "NGResetAcknowledge", "RANConfigurationUpdate", "RANConfigurationUpdateAcknowledge",
        "RANConfigurationUpdateFailure", "AMFConfigurationUpdate", "Paging",
        "UplinkRANConfigurationTransfer", "DownlinkRANConfigurationTransfer", "OverloadStop"};
    ogs_assert(ogs_ue_events_init("AMF", 128, 64));
    amf_gnb_t *a = make_gnb(sock, 1), *b = make_gnb(sock, 2);
    ogs_assert(a->event_association_id && a->event_association_id != b->event_association_id);
    amf_self()->amf_name = "ngap-test";
    amf_self()->num_of_served_guami = 1;
    ogs_plmn_id_build(&amf_self()->served_guami[0].plmn_id, 460, 11, 2);
    amf_self()->num_of_plmn_support = 1;
    amf_self()->plmn_support[0].plmn_id = amf_self()->served_guami[0].plmn_id;
    amf_self()->plmn_support[0].num_of_s_nssai = 1;
    amf_self()->plmn_support[0].s_nssai[0].sst = 1;
    amf_self()->plmn_support[0].s_nssai[0].sd.v = 0x010101;
    amf_ue_t ue = {0};
    char supi[] = "imsi-001010000000099";
    ue.supi = supi; ue.event_trace_id = ogs_ue_events_new_trace();
    ue.nr_tai.plmn_id = amf_self()->served_guami[0].plmn_id; ue.nr_tai.tac.v = 1;
    ue.current.guti.m_tmsi = 1234;
    for (unsigned i = 0; i < OGS_ARRAY_SIZE(names); i++) {
        ogs_pkbuf_t *bytes = event_fixture(i, &ue);
        ogs_assert(bytes);
        ogs_ngap_message_t decoded = {0};
        ogs_assert(ogs_ngap_decode(&decoded, bytes) == OGS_OK);
        uint64_t seq = amf_ue_event_ngap_for_ue(a, &decoded, "AMF->gNB", "queued", &ue);
        ogs_assert(seq);
        int status;
        char *text = ogs_ue_events_query(event_arg, NULL, &status);
        ogs_assert(text && status == 200);
        cJSON *root = cJSON_Parse(text); free(text);
        const cJSON *items = get(root, "items"), *event = cJSON_GetArrayItem(items, cJSON_GetArraySize(items) - 1);
        ogs_assert(!strcmp(get(event, "name")->valuestring, names[i]));
        ogs_assert(!strcmp(get(event, "scope")->valuestring, i == 12 ? "unknown" : "gnb"));
        ogs_assert(!strcmp(get(event, "supi")->valuestring, i == 9 ? ue.supi : ""));
        ogs_assert(cJSON_IsNull(get(event, "amf_ngap_id")));
        ogs_assert(cJSON_IsNull(get(event, "gnb_id")));
        ogs_assert(!strstr(get(event, "info")->valuestring, "gNB=0/"));
        if (i == 0) ogs_assert(strstr(get(event, "info")->valuestring, "reported-gNB=3584/22 PLMN=46011"));
        if (i == 8) ogs_assert(get(event, "procedure_code")->valueint == 0);
        if (i == 2 || i == 3 || i == 7) ogs_assert(get(event, "cause")->valuestring[0]);
        const char *directory = getenv("XCN_NGAP_FIXTURE_DIR");
        if (directory) {
            char path[1024];
            ogs_assert(snprintf(path, sizeof(path), "%s/%s.bin", directory, names[i]) < (int)sizeof(path));
            FILE *file = fopen(path, "wb"); ogs_assert(file);
            ogs_assert(fwrite(bytes->data, 1, bytes->len, file) == bytes->len && !fclose(file));
        }
        cJSON_Delete(root); ogs_ngap_free(&decoded); ogs_pkbuf_free(bytes);
    }
    /* Identical configured identities never join event lifetimes or inherit SUPI. */
    ogs_assert(amf_gnb_set_gnb_id(a, 3584, 22) == OGS_OK);
    ogs_assert(amf_gnb_set_gnb_id(b, 3584, 22) == OGS_OK);
    ogs_ngap_message_t message = {0};
    message.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    message.choice.initiatingMessage = ogs_calloc(1, sizeof(NGAP_InitiatingMessage_t));
    message.choice.initiatingMessage->value.present = NGAP_InitiatingMessage__value_PR_AMFConfigurationUpdate;
    amf_ue_event_ngap(b, &message, "gNB->AMF", "received");
    uint64_t old = b->event_association_id;
    amf_gnb_remove(b); b = make_gnb(sock, 2);
    ogs_assert(old != b->event_association_id);
    amf_ue_event_ngap(b, &message, "gNB->AMF", "received");
    /* Invalid discriminators, null alternatives and procedure values are harmless. */
    message.choice.initiatingMessage->procedureCode = -1;
    ogs_assert(!amf_ue_event_ngap(b, &message, "gNB->AMF", "received"));
    message.choice.initiatingMessage->procedureCode = 256;
    ogs_assert(!amf_ue_event_ngap(b, &message, "gNB->AMF", "received"));
    message.choice.initiatingMessage->procedureCode = 0;
    message.choice.initiatingMessage->value.present = 9999;
    ogs_assert(!amf_ue_event_ngap(b, &message, "gNB->AMF", "received"));
    message.choice.initiatingMessage->value.present = NGAP_InitiatingMessage__value_PR_NOTHING;
    ogs_ngap_free(&message);
    memset(&message, 0, sizeof(message));
    message.present = NGAP_NGAP_PDU_PR_successfulOutcome;
    ogs_assert(!amf_ue_event_ngap(b, &message, "gNB->AMF", "received"));
    amf_gnb_remove(b); amf_gnb_remove(a);
    ogs_ue_events_final();
}
