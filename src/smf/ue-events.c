/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ue-events.h"

static uint64_t record_event(smf_sess_t *sess, const char *protocol, const char *direction,
        const char *name, const char *result, const char *cause, const char *info, bool action,
        const ogs_ue_event_t *wire)
{
    if (!ogs_ue_events_enabled() || !sess || !sess->psi || !sess->event_trace_id) return 0;
    ogs_ue_event_t e = {.trace_id = sess->event_trace_id, .action = action,
        .flow_id = sess->event_flow_id, .psi = sess->psi, .pti = sess->pti,
        .amf_ngap_id = sess->xcn_amf_ue_ngap_id, .ran_ngap_id = sess->xcn_ran_ue_ngap_id,
        .upf_seid = sess->upf_n4_seid};
    smf_ue_t *ue = smf_ue_find_by_id(sess->smf_ue_id);
    e.has_ngap = e.amf_ngap_id != 0;
    if (ue) snprintf(e.supi, sizeof(e.supi), "%s", ue->supi ? ue->supi : "");
    snprintf(e.protocol, sizeof(e.protocol), "%s", protocol);
    snprintf(e.direction, sizeof(e.direction), "%s", direction ? direction : "");
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.result, sizeof(e.result), "%s", result);
    snprintf(e.cause, sizeof(e.cause), "%s", cause ? cause : "");
    snprintf(e.info, sizeof(e.info), "%s", info ? info : "");
    snprintf(e.dnn, sizeof(e.dnn), "%s", sess->session.name ? sess->session.name : "");
    smf_bearer_t *qos;
    size_t used = 0, flows = 0;
    char snapshot[sizeof(e.info)] = "";
    ogs_list_for_each(&sess->bearer_list, qos) {
        if (!qos->qfi) continue;
        if (!flows) { e.qfi = qos->qfi; e.fiveqi = qos->qos.index; }
        else { e.qfi = 0; e.fiveqi = 0; }
        flows++;
        int n = snprintf(snapshot + used, sizeof(snapshot)-used, "%s%u/%u",
                used ? "," : "QoS snapshot QFI/5QI=", qos->qfi, qos->qos.index);
        if (n < 0) break;
        if ((size_t)n >= sizeof(snapshot)-used) {
            memcpy(snapshot + sizeof(snapshot)-4, "...", 4);
            break;
        }
        used += n;
    }
    if ((!info || !*info) && flows) snprintf(e.info, sizeof(e.info), "%s", snapshot);
    if (wire) {
        e.psi = wire->psi; e.pti = wire->pti;
        if (wire->flow_id) e.flow_id = wire->flow_id;
        if (wire->qfi) { e.qfi = wire->qfi; e.fiveqi = wire->fiveqi; }
    }
    return ogs_ue_events_record(&e);
}

uint64_t smf_ue_event(smf_sess_t *sess, const char *protocol, const char *direction,
        const char *name, const char *result, const char *cause, const char *info, bool action)
{
    return record_event(sess, protocol, direction, name, result, cause, info, action, NULL);
}

void smf_ue_event_qos(smf_sess_t *sess, const char *result, unsigned qfi, unsigned fiveqi, const char *info)
{
    ogs_ue_event_t wire = {.psi = sess->psi, .pti = sess->pti, .qfi = qfi, .fiveqi = fiveqi};
    record_event(sess, "ACTION", "LOCAL", "QoSModification", result, NULL, info, true, &wire);
}

void smf_ue_event_establishment_end(smf_sess_t *sess, const char *result, const char *info)
{
    if (!sess || !sess->event_establishment.pending) return;
    ogs_ue_event_t wire = {.psi = sess->psi, .pti = sess->pti,
        .flow_id = sess->event_establishment.flow_id};
    memset(&sess->event_establishment, 0, sizeof(sess->event_establishment));
    record_event(sess, "ACTION", "LOCAL", "SessionEstablishment", result,
            NULL, info, true, &wire);
}

void smf_ue_event_establishment_n4(smf_sess_t *sess, ogs_pool_id_t xact_id,
        const char *result, const char *info)
{
    if (sess && xact_id != OGS_INVALID_POOL_ID &&
            sess->event_establishment.xact_id == xact_id)
        smf_ue_event_establishment_end(sess, result, info);
}

uint64_t smf_ue_event_nas(smf_sess_t *sess, ogs_nas_5gs_message_t *message, bool received)
{
    if (!ogs_ue_events_enabled() || !sess || !message) return 0;
    const char *name = "Unknown5GSM", *action = NULL, *result = NULL;
    char cause[96] = "";
    switch (message->gsm.h.message_type) {
    case OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REQUEST:
        name = "PDUSessionEstablishmentRequest"; action = "SessionEstablishment"; result = "started"; break;
    case OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_ACCEPT: name = "PDUSessionEstablishmentAccept"; break;
    case OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REJECT:
        name = "PDUSessionEstablishmentReject"; action = "SessionEstablishment"; result = "rejected";
        snprintf(cause, sizeof(cause), "5GSM cause=%u", message->gsm.pdu_session_establishment_reject.gsm_cause); break;
    case OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REQUEST:
        name = "PDUSessionModificationRequest"; action = "SessionModification"; result = "started"; break;
    case OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND:
        name = "PDUSessionModificationCommand"; action = "SessionModification"; result = "command-prepared"; break;
    case OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMPLETE: name = "PDUSessionModificationComplete"; break;
    case OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REJECT:
        name = "PDUSessionModificationReject"; action = "SessionModification"; result = "rejected";
        snprintf(cause, sizeof(cause), "5GSM cause=%u", message->gsm.pdu_session_modification_reject.gsm_cause); break;
    case OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND_REJECT:
        name = "PDUSessionModificationCommandReject";
        snprintf(cause, sizeof(cause), "5GSM cause=%u", message->gsm.pdu_session_modification_command_reject.gsm_cause); break;
    case OGS_NAS_5GS_PDU_SESSION_RELEASE_REQUEST:
        name = "PDUSessionReleaseRequest"; action = "SessionRelease"; result = "started"; break;
    case OGS_NAS_5GS_PDU_SESSION_RELEASE_COMMAND: name = "PDUSessionReleaseCommand"; break;
    case OGS_NAS_5GS_PDU_SESSION_RELEASE_COMPLETE: name = "PDUSessionReleaseComplete"; break;
    case OGS_NAS_5GS_PDU_SESSION_RELEASE_REJECT:
        name = "PDUSessionReleaseReject"; action = "SessionRelease"; result = "rejected";
        snprintf(cause, sizeof(cause), "5GSM cause=%u", message->gsm.pdu_session_release_reject.gsm_cause); break;
    case OGS_NAS_5GS_5GSM_STATUS: name = "5GSMStatus"; break;
    default: break;
    }
    if (received && action && !strcmp(result, "started")) sess->event_flow_id = ogs_ue_events_new_trace();
    if (received && message->gsm.h.message_type == OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REQUEST) {
        memset(&sess->event_establishment, 0, sizeof(sess->event_establishment));
        sess->event_establishment.pending = true;
        sess->event_establishment.flow_id = sess->event_flow_id;
    } else if (received && message->gsm.h.message_type == OGS_NAS_5GS_PDU_SESSION_RELEASE_REQUEST) {
        smf_ue_event_establishment_end(sess, "cancelled", "Session released before initial N2/N4 setup completed");
    } else if (message->gsm.h.message_type == OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_REJECT) {
        memset(&sess->event_establishment, 0, sizeof(sess->event_establishment));
    }
    ogs_ue_event_t wire = {.psi = message->gsm.h.pdu_session_identity, .pti = message->gsm.h.procedure_transaction_identity};
    uint64_t seq = record_event(sess, "NAS-5GSM", received ? "UE->SMF" : "SMF->UE", name,
            received ? "received" : "prepared", cause, NULL, false, &wire);
    if (action) smf_ue_event(sess, "ACTION", received ? "UE->SMF" : "SMF->UE",
            action, result, cause, NULL, true);
    return seq;
}

ogs_pkbuf_t *smf_ue_event_nas_encode(smf_sess_t *sess, ogs_nas_5gs_message_t *message)
{
    uint64_t seq = smf_ue_event_nas(sess, message, false);
    ogs_pkbuf_t *p = ogs_nas_5gs_plain_encode(message);
    if (p) ogs_ue_trace_record(seq, OGS_TRACE_5GSM, OGS_TRACE_PLAIN | OGS_TRACE_PREPARED, p->data, p->len);
    return p;
}

void smf_ue_event_pfcp(smf_sess_t *sess, uint8_t type, const char *result, uint8_t cause)
{
    const char *name = NULL;
    switch (type) {
    case OGS_PFCP_SESSION_ESTABLISHMENT_REQUEST_TYPE: name = "SessionEstablishmentRequest"; break;
    case OGS_PFCP_SESSION_ESTABLISHMENT_RESPONSE_TYPE: name = "SessionEstablishmentResponse"; break;
    case OGS_PFCP_SESSION_MODIFICATION_REQUEST_TYPE: name = "SessionModificationRequest"; break;
    case OGS_PFCP_SESSION_MODIFICATION_RESPONSE_TYPE: name = "SessionModificationResponse"; break;
    case OGS_PFCP_SESSION_DELETION_REQUEST_TYPE: name = "SessionDeletionRequest"; break;
    case OGS_PFCP_SESSION_DELETION_RESPONSE_TYPE: name = "SessionDeletionResponse"; break;
    default: return;
    }
    char text[96] = "";
    if (cause) snprintf(text, sizeof(text), "PFCP cause=%u %s", cause,
            cause == OGS_PFCP_CAUSE_REQUEST_ACCEPTED ? "Request accepted" : ogs_pfcp_cause_get_name(cause));
    smf_ue_event(sess, "PFCP", type % 2 ? "UPF->SMF" : "SMF->UPF", name, result, text, NULL, false);
    if (cause) {
        const char *action = type == OGS_PFCP_SESSION_ESTABLISHMENT_RESPONSE_TYPE ? "N4SessionEstablishment" :
            type == OGS_PFCP_SESSION_DELETION_RESPONSE_TYPE ? "N4SessionDeletion" : "N4SessionModification";
        smf_ue_event(sess, "ACTION", "UPF->SMF", action,
                cause == OGS_PFCP_CAUSE_REQUEST_ACCEPTED ? "accepted" : "rejected", text, NULL, true);
    }
}

static bool equal(const char *a, const char *b)
{
    return a && !strcmp(a, b);
}

static void record_sbi(smf_sess_t *sess, const ogs_sbi_header_t *h,
        int status, bool received, const char *result)
{
    const char *peer = "peer", *operation = "SBI";
    const char *resource = h->resource.component[0];
    const char *subresource = h->resource.component[1];
    const char *action = h->resource.component[2];
    bool response = status != 0;
    char name[64], direction[24], info[256];

    if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION)) {
        peer = equal(resource, OGS_SBI_RESOURCE_NAME_SM_CONTEXTS) ? "AMF" : "SMF";
        if (equal(resource, OGS_SBI_RESOURCE_NAME_SM_CONTEXTS)) {
            if (equal(action, OGS_SBI_RESOURCE_NAME_MODIFY)) operation = "UpdateSMContext";
            else if (equal(action, OGS_SBI_RESOURCE_NAME_RELEASE)) operation = "ReleaseSMContext";
            else if (equal(h->method, OGS_SBI_HTTP_METHOD_POST) && !subresource) operation = "CreateSMContext";
            else if (equal(h->method, OGS_SBI_HTTP_METHOD_GET)) operation = "RetrieveSMContext";
        } else if (equal(resource, OGS_SBI_RESOURCE_NAME_PDU_SESSIONS)) {
            if (equal(action, OGS_SBI_RESOURCE_NAME_MODIFY)) operation = "UpdatePDUSession";
            else if (equal(action, OGS_SBI_RESOURCE_NAME_RELEASE)) operation = "ReleasePDUSession";
            else if (!subresource) operation = "CreatePDUSession";
        }
    } else if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NUDM_SDM)) {
        peer = "UDM";
        if (equal(subresource, OGS_SBI_RESOURCE_NAME_SM_DATA)) operation = "GetSMData";
        else if (equal(subresource, OGS_SBI_RESOURCE_NAME_SDM_SUBSCRIPTIONS))
            operation = equal(h->method, OGS_SBI_HTTP_METHOD_DELETE) ? "DeleteSDMSubscription" : "CreateSDMSubscription";
    } else if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NUDM_UECM)) {
        peer = "UDM";
        operation = equal(h->method, OGS_SBI_HTTP_METHOD_DELETE) ? "DeregisterSMF" : "RegisterSMF";
    } else if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NPCF_SMPOLICYCONTROL)) {
        peer = "PCF";
        if (equal(resource, OGS_SBI_RESOURCE_NAME_SM_POLICIES)) {
            if (equal(action, OGS_SBI_RESOURCE_NAME_DELETE)) operation = "DeleteSMPolicy";
            else if (equal(action, OGS_SBI_RESOURCE_NAME_UPDATE)) operation = "UpdateSMPolicy";
            else if (!subresource) operation = "CreateSMPolicy";
        }
    } else if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NAMF_COMM)) {
        peer = "AMF";
        if (equal(resource, OGS_SBI_RESOURCE_NAME_UE_CONTEXTS) &&
                equal(action, OGS_SBI_RESOURCE_NAME_N1_N2_MESSAGES))
            operation = "N1N2MessageTransfer";
    }
    snprintf(name, sizeof(name), "%s%s", operation, response ? "Response" : "Request");
    /* nsmf-pdusession responses received here come from another SMF. */
    if (equal(h->service.name, OGS_SBI_SERVICE_NAME_NSMF_PDUSESSION) &&
            response == received) peer = "SMF";
    snprintf(direction, sizeof(direction), "%s->%s", received ? peer : "SMF", received ? "SMF" : peer);
    snprintf(info, sizeof(info), "%s /%s", h->method ? h->method : "", h->service.name ? h->service.name : "");
    if (h->api.version)
        snprintf(info + strlen(info), sizeof(info) - strlen(info), "/%s", h->api.version);
    for (size_t i = 0; i < OGS_SBI_MAX_NUM_OF_RESOURCE_COMPONENT; i++) {
        if (!h->resource.component[i]) break;
        snprintf(info + strlen(info), sizeof(info) - strlen(info), "/%s", h->resource.component[i]);
    }
    if (status) snprintf(info + strlen(info), sizeof(info) - strlen(info), " HTTP=%d", status);
    smf_ue_event(sess, "SBI", direction, name, result, NULL, info, false);
}

void smf_ue_event_sbi(smf_sess_t *sess, ogs_sbi_message_t *message, bool received)
{
    if (!ogs_ue_events_enabled() || !sess || !message) return;
    record_sbi(sess, &message->h, message->res_status, received,
            message->res_status >= 400 ? "failed" : received ? "received" : "queued");
}

void smf_ue_event_sbi_request(smf_sess_t *sess, ogs_sbi_request_t *request, const char *result)
{
    if (!ogs_ue_events_enabled() || !sess || !request) return;
    if (request->h.service.name) {
        record_sbi(sess, &request->h, 0, false, result);
    } else if (request->h.uri) {
        /* Parse a private header; the request and URI remain owned by SBI. */
        ogs_sbi_message_t message = {0};
        ogs_sbi_header_t header = {.uri = request->h.uri};
        char *path = NULL;
        if ((header.uri[0] == '/' || ogs_sbi_getpath_from_uri(&path, header.uri)) &&
                ogs_sbi_parse_header(&message, &header) == OGS_OK) {
            message.h.method = request->h.method;
            record_sbi(sess, &message.h, 0, false, result);
        } else
            smf_ue_event(sess, "SBI", "SMF->peer", "SBIRequest", result, NULL, "Invalid SBI request URI", false);
        ogs_sbi_header_free(&header);
        ogs_free(path);
    }
}
