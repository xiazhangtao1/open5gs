/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef NODE_PROFILE_FIXTURE_H
#define NODE_PROFILE_FIXTURE_H
#include "ogs-ngap.h"

/* Standard NG Setup fixture shared by the handler test and SCTP probe. */
static inline void node_profile_setup_fixture(ogs_ngap_message_t *pdu,
        const char *name, size_t length, int copies, uint32_t gnb_id)
{
    NGAP_NGSetupRequest_t *request;
    NGAP_NGSetupRequestIEs_t *ie;
    NGAP_GlobalGNB_ID_t *global;
    NGAP_SupportedTAItem_t *ta;
    NGAP_BroadcastPLMNItem_t *broadcast;
    NGAP_SliceSupportItem_t *slice;
    ogs_plmn_id_t plmn;
    ogs_uint24_t value;
    int i;

    memset(pdu, 0, sizeof(*pdu));
    ogs_plmn_id_build(&plmn, 460, 11, 2);
    pdu->present = NGAP_NGAP_PDU_PR_initiatingMessage;
    pdu->choice.initiatingMessage = CALLOC(1, sizeof(NGAP_InitiatingMessage_t));
    pdu->choice.initiatingMessage->procedureCode = NGAP_ProcedureCode_id_NGSetup;
    pdu->choice.initiatingMessage->criticality = NGAP_Criticality_reject;
    pdu->choice.initiatingMessage->value.present =
        NGAP_InitiatingMessage__value_PR_NGSetupRequest;
    request = &pdu->choice.initiatingMessage->value.choice.NGSetupRequest;

    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_GlobalRANNodeID;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_NGSetupRequestIEs__value_PR_GlobalRANNodeID;
    ie->value.choice.GlobalRANNodeID.present = NGAP_GlobalRANNodeID_PR_globalGNB_ID;
    global = CALLOC(1, sizeof(*global));
    ie->value.choice.GlobalRANNodeID.choice.globalGNB_ID = global;
    ogs_asn_buffer_to_OCTET_STRING(&plmn, OGS_PLMN_ID_LEN, &global->pLMNIdentity);
    ogs_ngap_uint32_to_GNB_ID(gnb_id, 22, &global->gNB_ID);
    ASN_SEQUENCE_ADD(&request->protocolIEs, ie);

    for (i = 0; i < copies; i++) {
        ie = CALLOC(1, sizeof(*ie));
        ie->id = NGAP_ProtocolIE_ID_id_RANNodeName;
        ie->criticality = NGAP_Criticality_ignore;
        ie->value.present = NGAP_NGSetupRequestIEs__value_PR_RANNodeName;
        ogs_asn_buffer_to_OCTET_STRING((void *)name, length,
                &ie->value.choice.RANNodeName);
        ASN_SEQUENCE_ADD(&request->protocolIEs, ie);
    }

    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_SupportedTAList;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_NGSetupRequestIEs__value_PR_SupportedTAList;
    ta = CALLOC(1, sizeof(*ta));
    value.v = 1;
    ogs_asn_uint24_to_OCTET_STRING(value, &ta->tAC);
    broadcast = CALLOC(1, sizeof(*broadcast));
    ogs_asn_buffer_to_OCTET_STRING(&plmn, OGS_PLMN_ID_LEN, &broadcast->pLMNIdentity);
    slice = CALLOC(1, sizeof(*slice));
    ogs_asn_uint8_to_OCTET_STRING(1, &slice->s_NSSAI.sST);
    slice->s_NSSAI.sD = CALLOC(1, sizeof(*slice->s_NSSAI.sD));
    value.v = 0x010101;
    ogs_asn_uint24_to_OCTET_STRING(value, slice->s_NSSAI.sD);
    ASN_SEQUENCE_ADD(&broadcast->tAISliceSupportList, slice);
    ASN_SEQUENCE_ADD(&ta->broadcastPLMNList, broadcast);
    ASN_SEQUENCE_ADD(&ie->value.choice.SupportedTAList, ta);
    ASN_SEQUENCE_ADD(&request->protocolIEs, ie);

    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_DefaultPagingDRX;
    ie->criticality = NGAP_Criticality_ignore;
    ie->value.present = NGAP_NGSetupRequestIEs__value_PR_PagingDRX;
    ie->value.choice.PagingDRX = NGAP_PagingDRX_v128;
    ASN_SEQUENCE_ADD(&request->protocolIEs, ie);
}
#endif
