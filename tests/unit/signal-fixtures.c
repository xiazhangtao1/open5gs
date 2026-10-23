/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Generate synthetic, non-subscriber PDUs using the same codecs as the NFs. */
#include "ogs-ngap.h"
#include "ogs-nas-5gs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void save(const char *dir, const char *name, ogs_pkbuf_t *p)
{
    char path[1024];
    ogs_assert(p);
    ogs_assert(snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path));
    FILE *f = fopen(path, "wb"); ogs_assert(f);
    ogs_assert(fwrite(p->data, 1, p->len, f) == p->len);
    ogs_assert(!fclose(f)); ogs_pkbuf_free(p);
}

static void session_fixture(const char *dir)
{
    NGAP_PDUSessionResourceSetupRequestTransfer_t transfer = {0};
    NGAP_PDUSessionResourceSetupRequestTransferIEs_t *t = CALLOC(1, sizeof(*t));
    t->id = NGAP_ProtocolIE_ID_id_UL_NGU_UP_TNLInformation;
    t->criticality = NGAP_Criticality_reject;
    t->value.present = NGAP_PDUSessionResourceSetupRequestTransferIEs__value_PR_UPTransportLayerInformation;
    t->value.choice.UPTransportLayerInformation.present = NGAP_UPTransportLayerInformation_PR_gTPTunnel;
    NGAP_GTPTunnel_t *tunnel = t->value.choice.UPTransportLayerInformation.choice.gTPTunnel = CALLOC(1, sizeof(*tunnel));
    tunnel->transportLayerAddress.buf = CALLOC(4, 1);
    tunnel->transportLayerAddress.size = 4;
    const uint8_t ip[] = {192, 0, 2, 1};
    memcpy(tunnel->transportLayerAddress.buf, ip, 4);
    ogs_asn_uint32_to_OCTET_STRING(0xdeadbeef, &tunnel->gTP_TEID);
    ASN_SEQUENCE_ADD(&transfer.protocolIEs, t);
    t = CALLOC(1, sizeof(*t));
    t->id = NGAP_ProtocolIE_ID_id_PDUSessionType;
    t->criticality = NGAP_Criticality_reject;
    t->value.present = NGAP_PDUSessionResourceSetupRequestTransferIEs__value_PR_PDUSessionType;
    t->value.choice.PDUSessionType = NGAP_PDUSessionType_ipv4;
    ASN_SEQUENCE_ADD(&transfer.protocolIEs, t);
    t = CALLOC(1, sizeof(*t));
    t->id = NGAP_ProtocolIE_ID_id_QosFlowSetupRequestList;
    t->criticality = NGAP_Criticality_reject;
    t->value.present = NGAP_PDUSessionResourceSetupRequestTransferIEs__value_PR_QosFlowSetupRequestList;
    NGAP_QosFlowSetupRequestItem_t *flow = CALLOC(1, sizeof(*flow));
    flow->qosFlowIdentifier = 1;
    flow->qosFlowLevelQosParameters.qosCharacteristics.present = NGAP_QosCharacteristics_PR_nonDynamic5QI;
    flow->qosFlowLevelQosParameters.qosCharacteristics.choice.nonDynamic5QI = CALLOC(1, sizeof(NGAP_NonDynamic5QIDescriptor_t));
    flow->qosFlowLevelQosParameters.qosCharacteristics.choice.nonDynamic5QI->fiveQI = 9;
    flow->qosFlowLevelQosParameters.allocationAndRetentionPriority.priorityLevelARP = 8;
    ASN_SEQUENCE_ADD(&t->value.choice.QosFlowSetupRequestList, flow);
    ASN_SEQUENCE_ADD(&transfer.protocolIEs, t);
    ogs_pkbuf_t *encoded = ogs_asn_encode(&asn_DEF_NGAP_PDUSessionResourceSetupRequestTransfer, &transfer);
    ogs_assert(encoded);

    NGAP_NGAP_PDU_t pdu = {0};
    pdu.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage = CALLOC(1, sizeof(*pdu.choice.initiatingMessage));
    NGAP_InitiatingMessage_t *msg = pdu.choice.initiatingMessage;
    msg->procedureCode = NGAP_ProcedureCode_id_PDUSessionResourceSetup;
    msg->criticality = NGAP_Criticality_reject;
    msg->value.present = NGAP_InitiatingMessage__value_PR_PDUSessionResourceSetupRequest;
    NGAP_PDUSessionResourceSetupRequest_t *setup = &msg->value.choice.PDUSessionResourceSetupRequest;
    NGAP_PDUSessionResourceSetupRequestIEs_t *ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_AMF_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_PDUSessionResourceSetupRequestIEs__value_PR_AMF_UE_NGAP_ID;
    ogs_assert(!asn_uint642INTEGER(&ie->value.choice.AMF_UE_NGAP_ID, 123));
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_PDUSessionResourceSetupRequestIEs__value_PR_RAN_UE_NGAP_ID;
    ie->value.choice.RAN_UE_NGAP_ID = 456;
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_PDUSessionResourceSetupListSUReq;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_PDUSessionResourceSetupRequestIEs__value_PR_PDUSessionResourceSetupListSUReq;
    NGAP_PDUSessionResourceSetupItemSUReq_t *item = CALLOC(1, sizeof(*item));
    item->pDUSessionID = 2;
    item->s_NSSAI.sST.buf = CALLOC(1, 1); item->s_NSSAI.sST.buf[0] = 1; item->s_NSSAI.sST.size = 1;
    ogs_asn_buffer_to_OCTET_STRING(encoded->data, encoded->len, &item->pDUSessionResourceSetupRequestTransfer);
    ogs_pkbuf_free(encoded);
    ASN_SEQUENCE_ADD(&ie->value.choice.PDUSessionResourceSetupListSUReq, item);
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    save(dir, "session-setup-transfer.bin", ogs_ngap_encode(&pdu));
}

int main(int argc, char **argv)
{
    ogs_assert(argc == 2);
    ogs_core_initialize();
    ogs_log_add_stderr();
    ogs_log_install_domain(&__ogs_ngap_domain, "ngap", OGS_LOG_ERROR);
    ogs_log_install_domain(&__ogs_nas_domain, "nas", OGS_LOG_ERROR);
    ogs_pkbuf_config_t config;
    ogs_pkbuf_default_init(&config); ogs_pkbuf_default_create(&config);
    ogs_nas_5gs_message_t nas = {0};
    nas.gmm.h.extended_protocol_discriminator = OGS_NAS_EXTENDED_PROTOCOL_DISCRIMINATOR_5GMM;
    nas.gmm.h.message_type = OGS_NAS_5GS_REGISTRATION_COMPLETE;
    save(argv[1], "registration-complete.bin", ogs_nas_5gs_plain_encode(&nas));
    memset(&nas, 0, sizeof(nas));
    nas.gsm.h.extended_protocol_discriminator = OGS_NAS_EXTENDED_PROTOCOL_DISCRIMINATOR_5GSM;
    nas.gsm.h.pdu_session_identity = 2;
    nas.gsm.h.procedure_transaction_identity = 1;
    nas.gsm.h.message_type = OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REQUEST;
    nas.gsm.pdu_session_establishment_request.integrity_protection_maximum_data_rate.ul = 0xff;
    nas.gsm.pdu_session_establishment_request.integrity_protection_maximum_data_rate.dl = 0xff;
    save(argv[1], "session-establishment-request.bin", ogs_nas_5gs_plain_encode(&nas));
    const uint8_t sm[] = {0x2e, 2, 1, 0xc1, 0xff, 0xff};
    memset(&nas, 0, sizeof(nas));
    nas.gmm.h.extended_protocol_discriminator = OGS_NAS_EXTENDED_PROTOCOL_DISCRIMINATOR_5GMM;
    nas.gmm.h.message_type = OGS_NAS_5GS_UL_NAS_TRANSPORT;
    nas.gmm.ul_nas_transport.payload_container_type.value = OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION;
    nas.gmm.ul_nas_transport.payload_container.length = sizeof(sm);
    nas.gmm.ul_nas_transport.payload_container.buffer = (void *)sm;
    save(argv[1], "ul-nas-transport.bin", ogs_nas_5gs_plain_encode(&nas));

    NGAP_NGAP_PDU_t pdu = {0};
    pdu.present = NGAP_NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage = CALLOC(1, sizeof(*pdu.choice.initiatingMessage));
    NGAP_InitiatingMessage_t *msg = pdu.choice.initiatingMessage;
    msg->procedureCode = NGAP_ProcedureCode_id_InitialContextSetup;
    msg->criticality = NGAP_Criticality_reject;
    msg->value.present = NGAP_InitiatingMessage__value_PR_InitialContextSetupRequest;
    NGAP_InitialContextSetupRequest_t *setup = &msg->value.choice.InitialContextSetupRequest;
    NGAP_InitialContextSetupRequestIEs_t *ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_AMF_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_InitialContextSetupRequestIEs__value_PR_AMF_UE_NGAP_ID;
    ogs_assert(!asn_uint642INTEGER(&ie->value.choice.AMF_UE_NGAP_ID, 1099511627775ull));
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_RAN_UE_NGAP_ID;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_InitialContextSetupRequestIEs__value_PR_RAN_UE_NGAP_ID;
    ie->value.choice.RAN_UE_NGAP_ID = 4294967295u;
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    ie = CALLOC(1, sizeof(*ie));
    ie->id = NGAP_ProtocolIE_ID_id_SecurityKey;
    ie->criticality = NGAP_Criticality_reject;
    ie->value.present = NGAP_InitialContextSetupRequestIEs__value_PR_SecurityKey;
    ie->value.choice.SecurityKey.buf = CALLOC(32, 1);
    ie->value.choice.SecurityKey.size = 32;
    memset(ie->value.choice.SecurityKey.buf, 0xa5, 32); /* Synthetic marker, never a real key. */
    ASN_SEQUENCE_ADD(&setup->protocolIEs, ie);
    save(argv[1], "initial-context-key.bin", ogs_ngap_encode(&pdu));
    session_fixture(argv[1]);
    ogs_pkbuf_default_destroy();
    ogs_core_terminate();
    return 0;
}
