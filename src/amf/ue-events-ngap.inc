/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Typed extraction for the UE procedures handled by this AMF. */
static ngap_meta_t ngap_metadata(ogs_ngap_message_t *pdu)
{
    ngap_meta_t m = {0};
    switch (pdu->present) {
    case NGAP_NGAP_PDU_PR_initiatingMessage: {
        NGAP_InitiatingMessage_t *p = pdu->choice.initiatingMessage;
        if (!p) break;
        switch (p->value.present) {
        case NGAP_InitiatingMessage__value_PR_HandoverCancel:
            m.name = "HandoverCancel";
            for (int i = 0; i < p->value.choice.HandoverCancel.protocolIEs.list.count; i++) {
                NGAP_HandoverCancelIEs_t *ie = p->value.choice.HandoverCancel.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverCancelIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverCancelIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_HandoverRequired:
            m.name = "HandoverRequired";
            for (int i = 0; i < p->value.choice.HandoverRequired.protocolIEs.list.count; i++) {
                NGAP_HandoverRequiredIEs_t *ie = p->value.choice.HandoverRequired.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverRequiredIEs__value_PR_PDUSessionResourceListHORqd) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceListHORqd.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceListHORqd.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceListHORqd.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverRequiredIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverRequiredIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
                if (ie->value.present == NGAP_HandoverRequiredIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_HandoverRequest:
            m.name = "HandoverRequest";
            for (int i = 0; i < p->value.choice.HandoverRequest.protocolIEs.list.count; i++) {
                NGAP_HandoverRequestIEs_t *ie = p->value.choice.HandoverRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverRequestIEs__value_PR_PDUSessionResourceSetupListHOReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSetupListHOReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSetupListHOReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSetupListHOReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverRequestIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_InitialContextSetupRequest:
            m.name = "InitialContextSetupRequest";
            for (int i = 0; i < p->value.choice.InitialContextSetupRequest.protocolIEs.list.count; i++) {
                NGAP_InitialContextSetupRequestIEs_t *ie = p->value.choice.InitialContextSetupRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_InitialContextSetupRequestIEs__value_PR_PDUSessionResourceSetupListCxtReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSetupListCxtReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSetupListCxtReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSetupListCxtReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_InitialContextSetupRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_InitialContextSetupRequestIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_PathSwitchRequest:
            m.name = "PathSwitchRequest";
            for (int i = 0; i < p->value.choice.PathSwitchRequest.protocolIEs.list.count; i++) {
                NGAP_PathSwitchRequestIEs_t *ie = p->value.choice.PathSwitchRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PathSwitchRequestIEs__value_PR_PDUSessionResourceToBeSwitchedDLList) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceToBeSwitchedDLList.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceToBeSwitchedDLList.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceToBeSwitchedDLList.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PathSwitchRequestIEs__value_PR_PDUSessionResourceFailedToSetupListPSReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToSetupListPSReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToSetupListPSReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToSetupListPSReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PathSwitchRequestIEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_PathSwitchRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PathSwitchRequestIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_PDUSessionResourceModifyRequest:
            m.name = "PDUSessionResourceModifyRequest";
            for (int i = 0; i < p->value.choice.PDUSessionResourceModifyRequest.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceModifyRequestIEs_t *ie = p->value.choice.PDUSessionResourceModifyRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceModifyRequestIEs__value_PR_PDUSessionResourceModifyListModReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceModifyListModReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceModifyListModReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceModifyListModReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceModifyRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceModifyRequestIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_PDUSessionResourceReleaseCommand:
            m.name = "PDUSessionResourceReleaseCommand";
            for (int i = 0; i < p->value.choice.PDUSessionResourceReleaseCommand.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceReleaseCommandIEs_t *ie = p->value.choice.PDUSessionResourceReleaseCommand.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceReleaseCommandIEs__value_PR_PDUSessionResourceToReleaseListRelCmd) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceToReleaseListRelCmd.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceToReleaseListRelCmd.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceToReleaseListRelCmd.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceReleaseCommandIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceReleaseCommandIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_PDUSessionResourceSetupRequest:
            m.name = "PDUSessionResourceSetupRequest";
            for (int i = 0; i < p->value.choice.PDUSessionResourceSetupRequest.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceSetupRequestIEs_t *ie = p->value.choice.PDUSessionResourceSetupRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceSetupRequestIEs__value_PR_PDUSessionResourceSetupListSUReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSetupListSUReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSetupListSUReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSetupListSUReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceSetupRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceSetupRequestIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UEContextModificationRequest:
            m.name = "UEContextModificationRequest";
            for (int i = 0; i < p->value.choice.UEContextModificationRequest.protocolIEs.list.count; i++) {
                NGAP_UEContextModificationRequestIEs_t *ie = p->value.choice.UEContextModificationRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextModificationRequestIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UEContextModificationRequestIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UEContextReleaseCommand:
            m.name = "UEContextReleaseCommand";
            for (int i = 0; i < p->value.choice.UEContextReleaseCommand.protocolIEs.list.count; i++) {
                NGAP_UEContextReleaseCommand_IEs_t *ie = p->value.choice.UEContextReleaseCommand.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextReleaseCommand_IEs__value_PR_UE_NGAP_IDs) {
                    NGAP_UE_NGAP_IDs_t *ids = &ie->value.choice.UE_NGAP_IDs;
                    if (ids->present == NGAP_UE_NGAP_IDs_PR_uE_NGAP_ID_pair && ids->choice.uE_NGAP_ID_pair) {
                        m.has_amf = asn_INTEGER2uint64(&ids->choice.uE_NGAP_ID_pair->aMF_UE_NGAP_ID, &m.amf_id) == 0;
                        m.ran_id = ids->choice.uE_NGAP_ID_pair->rAN_UE_NGAP_ID; m.has_ran = true;
                    } else if (ids->present == NGAP_UE_NGAP_IDs_PR_aMF_UE_NGAP_ID) {
                        m.has_amf = asn_INTEGER2uint64(&ids->choice.aMF_UE_NGAP_ID, &m.amf_id) == 0;
                    }
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_DownlinkNASTransport:
            m.name = "DownlinkNASTransport";
            for (int i = 0; i < p->value.choice.DownlinkNASTransport.protocolIEs.list.count; i++) {
                NGAP_DownlinkNASTransport_IEs_t *ie = p->value.choice.DownlinkNASTransport.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_DownlinkNASTransport_IEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_DownlinkNASTransport_IEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_DownlinkRANStatusTransfer:
            m.name = "DownlinkRANStatusTransfer";
            for (int i = 0; i < p->value.choice.DownlinkRANStatusTransfer.protocolIEs.list.count; i++) {
                NGAP_DownlinkRANStatusTransferIEs_t *ie = p->value.choice.DownlinkRANStatusTransfer.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_DownlinkRANStatusTransferIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_DownlinkRANStatusTransferIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_ErrorIndication:
            m.name = "ErrorIndication";
            for (int i = 0; i < p->value.choice.ErrorIndication.protocolIEs.list.count; i++) {
                NGAP_ErrorIndicationIEs_t *ie = p->value.choice.ErrorIndication.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_ErrorIndicationIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_ErrorIndicationIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
                if (ie->value.present == NGAP_ErrorIndicationIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_HandoverNotify:
            m.name = "HandoverNotify";
            for (int i = 0; i < p->value.choice.HandoverNotify.protocolIEs.list.count; i++) {
                NGAP_HandoverNotifyIEs_t *ie = p->value.choice.HandoverNotify.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverNotifyIEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_HandoverNotifyIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverNotifyIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_InitialUEMessage:
            m.name = "InitialUEMessage";
            for (int i = 0; i < p->value.choice.InitialUEMessage.protocolIEs.list.count; i++) {
                NGAP_InitialUEMessage_IEs_t *ie = p->value.choice.InitialUEMessage.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_InitialUEMessage_IEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_InitialUEMessage_IEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UEContextReleaseRequest:
            m.name = "UEContextReleaseRequest";
            for (int i = 0; i < p->value.choice.UEContextReleaseRequest.protocolIEs.list.count; i++) {
                NGAP_UEContextReleaseRequest_IEs_t *ie = p->value.choice.UEContextReleaseRequest.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextReleaseRequest_IEs__value_PR_PDUSessionResourceListCxtRelReq) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceListCxtRelReq.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceListCxtRelReq.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceListCxtRelReq.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_UEContextReleaseRequest_IEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UEContextReleaseRequest_IEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UERadioCapabilityInfoIndication:
            m.name = "UERadioCapabilityInfoIndication";
            for (int i = 0; i < p->value.choice.UERadioCapabilityInfoIndication.protocolIEs.list.count; i++) {
                NGAP_UERadioCapabilityInfoIndicationIEs_t *ie = p->value.choice.UERadioCapabilityInfoIndication.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UERadioCapabilityInfoIndicationIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UERadioCapabilityInfoIndicationIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UplinkNASTransport:
            m.name = "UplinkNASTransport";
            for (int i = 0; i < p->value.choice.UplinkNASTransport.protocolIEs.list.count; i++) {
                NGAP_UplinkNASTransport_IEs_t *ie = p->value.choice.UplinkNASTransport.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UplinkNASTransport_IEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_UplinkNASTransport_IEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UplinkNASTransport_IEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_InitiatingMessage__value_PR_UplinkRANStatusTransfer:
            m.name = "UplinkRANStatusTransfer";
            for (int i = 0; i < p->value.choice.UplinkRANStatusTransfer.protocolIEs.list.count; i++) {
                NGAP_UplinkRANStatusTransferIEs_t *ie = p->value.choice.UplinkRANStatusTransfer.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UplinkRANStatusTransferIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UplinkRANStatusTransferIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        default: break;
        }
        break;
    }
    case NGAP_NGAP_PDU_PR_successfulOutcome: {
        NGAP_SuccessfulOutcome_t *p = pdu->choice.successfulOutcome;
        if (!p) break;
        switch (p->value.present) {
        case NGAP_SuccessfulOutcome__value_PR_HandoverCancelAcknowledge:
            m.name = "HandoverCancelAcknowledge";
            for (int i = 0; i < p->value.choice.HandoverCancelAcknowledge.protocolIEs.list.count; i++) {
                NGAP_HandoverCancelAcknowledgeIEs_t *ie = p->value.choice.HandoverCancelAcknowledge.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverCancelAcknowledgeIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverCancelAcknowledgeIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_HandoverCommand:
            m.name = "HandoverCommand";
            for (int i = 0; i < p->value.choice.HandoverCommand.protocolIEs.list.count; i++) {
                NGAP_HandoverCommandIEs_t *ie = p->value.choice.HandoverCommand.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverCommandIEs__value_PR_PDUSessionResourceHandoverList) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceHandoverList.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceHandoverList.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceHandoverList.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverCommandIEs__value_PR_PDUSessionResourceToReleaseListHOCmd) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceToReleaseListHOCmd.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceToReleaseListHOCmd.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceToReleaseListHOCmd.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverCommandIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverCommandIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_HandoverRequestAcknowledge:
            m.name = "HandoverRequestAcknowledge";
            for (int i = 0; i < p->value.choice.HandoverRequestAcknowledge.protocolIEs.list.count; i++) {
                NGAP_HandoverRequestAcknowledgeIEs_t *ie = p->value.choice.HandoverRequestAcknowledge.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverRequestAcknowledgeIEs__value_PR_PDUSessionResourceAdmittedList) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceAdmittedList.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceAdmittedList.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceAdmittedList.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverRequestAcknowledgeIEs__value_PR_PDUSessionResourceFailedToSetupListHOAck) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToSetupListHOAck.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToSetupListHOAck.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToSetupListHOAck.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_HandoverRequestAcknowledgeIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverRequestAcknowledgeIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_InitialContextSetupResponse:
            m.name = "InitialContextSetupResponse";
            for (int i = 0; i < p->value.choice.InitialContextSetupResponse.protocolIEs.list.count; i++) {
                NGAP_InitialContextSetupResponseIEs_t *ie = p->value.choice.InitialContextSetupResponse.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_InitialContextSetupResponseIEs__value_PR_PDUSessionResourceSetupListCxtRes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSetupListCxtRes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSetupListCxtRes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSetupListCxtRes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_InitialContextSetupResponseIEs__value_PR_PDUSessionResourceFailedToSetupListCxtRes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToSetupListCxtRes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToSetupListCxtRes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToSetupListCxtRes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_InitialContextSetupResponseIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_InitialContextSetupResponseIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_PathSwitchRequestAcknowledge:
            m.name = "PathSwitchRequestAcknowledge";
            for (int i = 0; i < p->value.choice.PathSwitchRequestAcknowledge.protocolIEs.list.count; i++) {
                NGAP_PathSwitchRequestAcknowledgeIEs_t *ie = p->value.choice.PathSwitchRequestAcknowledge.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_PDUSessionResourceSwitchedList) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSwitchedList.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSwitchedList.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSwitchedList.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_PDUSessionResourceReleasedListPSAck) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceReleasedListPSAck.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceReleasedListPSAck.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceReleasedListPSAck.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PathSwitchRequestAcknowledgeIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_PDUSessionResourceModifyResponse:
            m.name = "PDUSessionResourceModifyResponse";
            for (int i = 0; i < p->value.choice.PDUSessionResourceModifyResponse.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceModifyResponseIEs_t *ie = p->value.choice.PDUSessionResourceModifyResponse.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceModifyResponseIEs__value_PR_PDUSessionResourceModifyListModRes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceModifyListModRes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceModifyListModRes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceModifyListModRes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceModifyResponseIEs__value_PR_PDUSessionResourceFailedToModifyListModRes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToModifyListModRes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToModifyListModRes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToModifyListModRes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceModifyResponseIEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_PDUSessionResourceModifyResponseIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceModifyResponseIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_PDUSessionResourceReleaseResponse:
            m.name = "PDUSessionResourceReleaseResponse";
            for (int i = 0; i < p->value.choice.PDUSessionResourceReleaseResponse.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceReleaseResponseIEs_t *ie = p->value.choice.PDUSessionResourceReleaseResponse.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceReleaseResponseIEs__value_PR_PDUSessionResourceReleasedListRelRes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceReleasedListRelRes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceReleasedListRelRes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceReleasedListRelRes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceReleaseResponseIEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_PDUSessionResourceReleaseResponseIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceReleaseResponseIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_PDUSessionResourceSetupResponse:
            m.name = "PDUSessionResourceSetupResponse";
            for (int i = 0; i < p->value.choice.PDUSessionResourceSetupResponse.protocolIEs.list.count; i++) {
                NGAP_PDUSessionResourceSetupResponseIEs_t *ie = p->value.choice.PDUSessionResourceSetupResponse.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PDUSessionResourceSetupResponseIEs__value_PR_PDUSessionResourceSetupListSURes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceSetupListSURes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceSetupListSURes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceSetupListSURes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceSetupResponseIEs__value_PR_PDUSessionResourceFailedToSetupListSURes) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToSetupListSURes.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToSetupListSURes.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToSetupListSURes.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PDUSessionResourceSetupResponseIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PDUSessionResourceSetupResponseIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_UEContextModificationResponse:
            m.name = "UEContextModificationResponse";
            for (int i = 0; i < p->value.choice.UEContextModificationResponse.protocolIEs.list.count; i++) {
                NGAP_UEContextModificationResponseIEs_t *ie = p->value.choice.UEContextModificationResponse.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextModificationResponseIEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_UEContextModificationResponseIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UEContextModificationResponseIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_SuccessfulOutcome__value_PR_UEContextReleaseComplete:
            m.name = "UEContextReleaseComplete";
            for (int i = 0; i < p->value.choice.UEContextReleaseComplete.protocolIEs.list.count; i++) {
                NGAP_UEContextReleaseComplete_IEs_t *ie = p->value.choice.UEContextReleaseComplete.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextReleaseComplete_IEs__value_PR_PDUSessionResourceListCxtRelCpl) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceListCxtRelCpl.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceListCxtRelCpl.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceListCxtRelCpl.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_UEContextReleaseComplete_IEs__value_PR_UserLocationInformation)
                    m.location = &ie->value.choice.UserLocationInformation;
                if (ie->value.present == NGAP_UEContextReleaseComplete_IEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UEContextReleaseComplete_IEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        default: break;
        }
        break;
    }
    case NGAP_NGAP_PDU_PR_unsuccessfulOutcome: {
        NGAP_UnsuccessfulOutcome_t *p = pdu->choice.unsuccessfulOutcome;
        if (!p) break;
        switch (p->value.present) {
        case NGAP_UnsuccessfulOutcome__value_PR_HandoverPreparationFailure:
            m.name = "HandoverPreparationFailure";
            for (int i = 0; i < p->value.choice.HandoverPreparationFailure.protocolIEs.list.count; i++) {
                NGAP_HandoverPreparationFailureIEs_t *ie = p->value.choice.HandoverPreparationFailure.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverPreparationFailureIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverPreparationFailureIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
                if (ie->value.present == NGAP_HandoverPreparationFailureIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_UnsuccessfulOutcome__value_PR_HandoverFailure:
            m.name = "HandoverFailure";
            for (int i = 0; i < p->value.choice.HandoverFailure.protocolIEs.list.count; i++) {
                NGAP_HandoverFailureIEs_t *ie = p->value.choice.HandoverFailure.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_HandoverFailureIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_HandoverFailureIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_UnsuccessfulOutcome__value_PR_InitialContextSetupFailure:
            m.name = "InitialContextSetupFailure";
            for (int i = 0; i < p->value.choice.InitialContextSetupFailure.protocolIEs.list.count; i++) {
                NGAP_InitialContextSetupFailureIEs_t *ie = p->value.choice.InitialContextSetupFailure.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_InitialContextSetupFailureIEs__value_PR_PDUSessionResourceFailedToSetupListCxtFail) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceFailedToSetupListCxtFail.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceFailedToSetupListCxtFail.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceFailedToSetupListCxtFail.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_InitialContextSetupFailureIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_InitialContextSetupFailureIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
                if (ie->value.present == NGAP_InitialContextSetupFailureIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        case NGAP_UnsuccessfulOutcome__value_PR_PathSwitchRequestFailure:
            m.name = "PathSwitchRequestFailure";
            for (int i = 0; i < p->value.choice.PathSwitchRequestFailure.protocolIEs.list.count; i++) {
                NGAP_PathSwitchRequestFailureIEs_t *ie = p->value.choice.PathSwitchRequestFailure.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_PathSwitchRequestFailureIEs__value_PR_PDUSessionResourceReleasedListPSFail) {
                    for (int j = 0; j < ie->value.choice.PDUSessionResourceReleasedListPSFail.list.count; j++)
                        if (ie->value.choice.PDUSessionResourceReleasedListPSFail.list.array[j])
                            ngap_psi(&m, ie->value.choice.PDUSessionResourceReleasedListPSFail.list.array[j]->pDUSessionID);
                }
                if (ie->value.present == NGAP_PathSwitchRequestFailureIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_PathSwitchRequestFailureIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
            }
            break;
        case NGAP_UnsuccessfulOutcome__value_PR_UEContextModificationFailure:
            m.name = "UEContextModificationFailure";
            for (int i = 0; i < p->value.choice.UEContextModificationFailure.protocolIEs.list.count; i++) {
                NGAP_UEContextModificationFailureIEs_t *ie = p->value.choice.UEContextModificationFailure.protocolIEs.list.array[i];
                if (!ie) continue;
                if (ie->value.present == NGAP_UEContextModificationFailureIEs__value_PR_AMF_UE_NGAP_ID) {
                    if (asn_INTEGER2uint64(&ie->value.choice.AMF_UE_NGAP_ID, &m.amf_id) == 0) m.has_amf = true;
                }
                if (ie->value.present == NGAP_UEContextModificationFailureIEs__value_PR_RAN_UE_NGAP_ID) {
                    m.ran_id = ie->value.choice.RAN_UE_NGAP_ID; m.has_ran = true;
                }
                if (ie->value.present == NGAP_UEContextModificationFailureIEs__value_PR_Cause) {
                    m.cause = &ie->value.choice.Cause;
                }
            }
            break;
        default: break;
        }
        break;
    }
    default: break;
    }
    return m;
}
