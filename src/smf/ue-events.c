/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ue-events.h"

static void record_event(smf_sess_t *sess, const char *protocol, const char *direction,
        const char *name, const char *result, const char *cause, const char *info, bool action,
        const ogs_ue_event_t *wire)
{
    if (!ogs_ue_events_enabled() || !sess || !sess->psi || !sess->event_trace_id) return;
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
        if (wire->qfi) { e.qfi = wire->qfi; e.fiveqi = wire->fiveqi; }
    }
    ogs_ue_events_record(&e);
}

void smf_ue_event(smf_sess_t *sess, const char *protocol, const char *direction,
        const char *name, const char *result, const char *cause, const char *info, bool action)
{
    record_event(sess, protocol, direction, name, result, cause, info, action, NULL);
}

void smf_ue_event_qos(smf_sess_t *sess, const char *result, unsigned qfi, unsigned fiveqi, const char *info)
{
    ogs_ue_event_t wire = {.psi = sess->psi, .pti = sess->pti, .qfi = qfi, .fiveqi = fiveqi};
    record_event(sess, "ACTION", "SMF", "QoSModification", result, NULL, info, true, &wire);
}

void smf_ue_event_nas(smf_sess_t *sess, ogs_nas_5gs_message_t *message, bool received)
{
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
    ogs_ue_event_t wire = {.psi = message->gsm.h.pdu_session_identity, .pti = message->gsm.h.procedure_transaction_identity};
    record_event(sess, "NAS-5GSM", received ? "UE->SMF" : "SMF->UE", name,
            received ? "received" : "prepared", cause, NULL, false, &wire);
    if (action) smf_ue_event(sess, "ACTION", received ? "UE->SMF" : "SMF->UE",
            action, result, cause, NULL, true);
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
    if (cause) snprintf(text, sizeof(text), "PFCP cause=%u %s", cause, ogs_pfcp_cause_get_name(cause));
    smf_ue_event(sess, "PFCP", type % 2 ? "UPF->SMF" : "SMF->UPF", name, result, text, NULL, false);
    if (cause) {
        const char *action = type == OGS_PFCP_SESSION_ESTABLISHMENT_RESPONSE_TYPE ? "N4SessionEstablishment" :
            type == OGS_PFCP_SESSION_DELETION_RESPONSE_TYPE ? "N4SessionDeletion" : "N4SessionModification";
        smf_ue_event(sess, "ACTION", "UPF->SMF", action,
                cause == OGS_PFCP_CAUSE_REQUEST_ACCEPTED ? "accepted" : "rejected", text, NULL, true);
    }
}

void smf_ue_event_sbi(smf_sess_t *sess, ogs_sbi_message_t *message, bool received)
{
    if (!message) return;
    char info[256];
    snprintf(info, sizeof(info), "%s %s/%s/%s HTTP=%d", message->h.method ? message->h.method : "",
            message->h.service.name ? message->h.service.name : "",
            message->h.resource.component[0] ? message->h.resource.component[0] : "",
            message->h.resource.component[2] ? message->h.resource.component[2] : "",
            message->res_status);
    smf_ue_event(sess, "SBI", received ? "peer->SMF" : "SMF->peer", "SMContextTransaction",
            message->res_status >= 400 ? "failed" : "received", NULL, info, false);
}
