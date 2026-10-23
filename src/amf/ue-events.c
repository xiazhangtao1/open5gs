/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "ue-events.h"
#include <inttypes.h>

typedef struct {
    const char *name;
    uint64_t amf_id, ran_id;
    bool has_amf, has_ran;
    NGAP_Cause_t *cause;
    NGAP_UserLocationInformation_t *location;
    uint8_t psis[16], psi_count;
} ngap_meta_t;

static void ngap_psi(ngap_meta_t *m, long psi)
{
    if (psi < 1 || psi > 15 || m->psi_count >= sizeof(m->psis)) return;
    for (size_t i = 0; i < m->psi_count; i++) if (m->psis[i] == psi) return;
    m->psis[m->psi_count++] = psi;
}

static uint64_t record_event(ran_ue_t *ran, amf_ue_t *ue, const char *protocol,
        const char *direction, const char *name, const char *result,
        const char *cause, const char *info, bool action,
        amf_gnb_t *reported, const ngap_meta_t *wire)
{
    ogs_ue_event_t e = {0};
    if (!ogs_ue_events_enabled()) return 0;
    if (!ran && ue) ran = ran_ue_find_by_id(ue->ran_ue_id);
    if (!ue && ran) ue = amf_ue_find_by_id(ran->amf_ue_id);
    if (ran) {
        e.trace_id = ran->event_trace_id;
        e.parent_seq = ran->event_ngap_seq;
        e.has_ngap = true;
        e.amf_ngap_id = ran->amf_ue_ngap_id;
        e.ran_ngap_id = ran->ran_ue_ngap_id;
        amf_gnb_t *gnb = amf_gnb_find_by_id(ran->gnb_id);
        if (gnb) {
            e.has_gnb = gnb->gnb_id_presence;
            e.gnb_id = gnb->gnb_id; e.gnb_bits = gnb->gnb_id_bits;
            ogs_plmn_id_to_string(&gnb->plmn_id, e.plmn);
            snprintf(e.peer, sizeof(e.peer), "%s", ogs_sockaddr_to_string_static(gnb->sctp.addr));
        }
    } else if (ue) e.trace_id = ue->event_trace_id;
    if (!e.trace_id) e.trace_id = ogs_ue_events_new_trace();
    if (reported) {
        e.gnb_id = reported->gnb_id; e.gnb_bits = reported->gnb_id_bits;
        ogs_plmn_id_to_string(&reported->plmn_id, e.plmn);
        snprintf(e.peer, sizeof(e.peer), "%s", ogs_sockaddr_to_string_static(reported->sctp.addr));
        e.has_gnb = reported->gnb_id_presence;
    }
    if (ue) {
        e.flow_id = ue->event_flow_id;
        snprintf(e.supi, sizeof(e.supi), "%s", ue->supi ? ue->supi : "");
        e.has_location = ue->ue_location_timestamp != 0;
        e.nci = ue->nr_cgi.cell_id; e.tac = ue->nr_tai.tac.v;
        if (e.has_location) ogs_plmn_id_to_string(&ue->nr_cgi.plmn_id, e.cell_plmn);
    }
    if (ran && ran->saved.nr_cgi.cell_id) {
        e.has_location = true;
        e.nci = ran->saved.nr_cgi.cell_id; e.tac = ran->saved.nr_tai.tac.v;
        ogs_plmn_id_to_string(&ran->saved.nr_cgi.plmn_id, e.cell_plmn);
    }
    NGAP_UserLocationInformation_t *location = wire ? wire->location : NULL;
    if (wire) {
        if (wire->has_amf) { e.amf_ngap_id = wire->amf_id; e.has_ngap = true; }
        if (wire->has_ran) { e.ran_ngap_id = wire->ran_id; e.has_ngap = true; }
        e.psi_count = wire->psi_count;
        memcpy(e.psis, wire->psis, sizeof(e.psis));
        if (wire->psi_count == 1) e.psi = wire->psis[0];
    }
    if (location && location->present == NGAP_UserLocationInformation_PR_userLocationInformationNR &&
            location->choice.userLocationInformationNR &&
            location->choice.userLocationInformationNR->nR_CGI.pLMNIdentity.buf &&
            location->choice.userLocationInformationNR->nR_CGI.pLMNIdentity.size == OGS_PLMN_ID_LEN &&
            location->choice.userLocationInformationNR->nR_CGI.nRCellIdentity.buf &&
            location->choice.userLocationInformationNR->nR_CGI.nRCellIdentity.size == 5 &&
            location->choice.userLocationInformationNR->nR_CGI.nRCellIdentity.bits_unused == 4 &&
            location->choice.userLocationInformationNR->tAI.pLMNIdentity.buf &&
            location->choice.userLocationInformationNR->tAI.pLMNIdentity.size == OGS_PLMN_ID_LEN &&
            location->choice.userLocationInformationNR->tAI.tAC.buf &&
            location->choice.userLocationInformationNR->tAI.tAC.size == 3) {
        ogs_nr_cgi_t cgi; ogs_5gs_tai_t tai;
        ogs_ngap_ASN_to_nr_cgi(&location->choice.userLocationInformationNR->nR_CGI, &cgi);
        ogs_ngap_ASN_to_5gs_tai(&location->choice.userLocationInformationNR->tAI, &tai);
        e.nci = cgi.cell_id; e.tac = tai.tac.v; e.has_location = true;
        ogs_plmn_id_to_string(&cgi.plmn_id, e.cell_plmn);
    }
    if (!strcmp(protocol, "NGAP") || (direction && !strcmp(direction, "AMF->UE"))) e.parent_seq = 0;
    if (e.supi[0]) ogs_ue_events_bind(e.trace_id, e.supi);
    e.action = action;
    snprintf(e.protocol, sizeof(e.protocol), "%s", protocol);
    snprintf(e.direction, sizeof(e.direction), "%s", direction ? direction : "");
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.result, sizeof(e.result), "%s", result);
    snprintf(e.cause, sizeof(e.cause), "%s", cause ? cause : "");
    snprintf(e.info, sizeof(e.info), "%s", info ? info : "");
    return ogs_ue_events_record(&e);
}

uint64_t amf_ue_event(ran_ue_t *ran, amf_ue_t *ue, const char *protocol,
        const char *direction, const char *name, const char *result,
        const char *cause, const char *info, bool action)
{
    return record_event(ran, ue, protocol, direction, name, result, cause, info, action, NULL, NULL);
}

void amf_ue_event_session(amf_ue_t *ue, ran_ue_t *ran, amf_sess_t *sess,
        ogs_sbi_message_t *message, const char *name)
{
    if (!ogs_ue_events_enabled() || !ue || !sess || !message) return;
    ngap_meta_t wire = {.psi_count = 1, .psis = {sess->psi}};
    const char *context = sess->sm_context_ref ? sess->sm_context_ref : "-";
    char *path = NULL;
    ogs_sbi_message_t parsed = {0};
    ogs_sbi_header_t header = {0};
    /* RX is recorded before the handler stores the new context reference. */
    if (!strcmp(name, "CreateSMContextResponse") &&
            message->res_status == OGS_SBI_HTTP_STATUS_CREATED) {
        context = "missing-location";
        if (message->http.location && message->http.location[0]) {
            header.uri = message->http.location;
            context = "invalid-location";
            if ((header.uri[0] == '/' || ogs_sbi_getpath_from_uri(&path, header.uri)) &&
                    ogs_sbi_parse_header(&parsed, &header) == OGS_OK &&
                    parsed.h.resource.component[1] && parsed.h.resource.component[1][0])
                context = parsed.h.resource.component[1];
        }
    }
    char info[256];
    snprintf(info, sizeof(info), "PSI=%u DNN=%s HTTP=%d SM-context=%s", sess->psi,
            sess->dnn ? sess->dnn : "-", message->res_status, context);
    /* Header owns parsed components, but not the response's Location string. */
    ogs_sbi_header_free(&header);
    ogs_free(path);
    record_event(ran, ue, "SBI", "SMF->AMF", name,
            message->res_status >= 400 ? "failed" : "received", NULL, info, false, NULL, &wire);
}



/* Every case uses its generated ASN.1 type and checks the CHOICE discriminator. */
#include "ue-events-ngap.inc"

uint64_t amf_ue_event_ngap(amf_gnb_t *gnb, ogs_ngap_message_t *message,
        const char *direction, const char *result)
{
    if (!ogs_ue_events_enabled() || !gnb || !message) return 0;
    ngap_meta_t m = ngap_metadata(message);
    if (!m.name) return 0;
    ran_ue_t *ran = NULL;
    if (m.has_amf) {
        /* The implementation's pool index is 32-bit; never truncate a wire ID. */
        if (m.amf_id <= UINT32_MAX) ran = ran_ue_find_by_amf_ue_ngap_id(m.amf_id);
        if (ran && strcmp(m.name, "PathSwitchRequest") &&
                (ran->gnb_id != gnb->id || (m.has_ran && ran->ran_ue_ngap_id != m.ran_id)))
            ran = NULL;
    } else if (m.has_ran) ran = ran_ue_find_by_ran_ue_ngap_id(gnb, m.ran_id);
    /* InitialUEMessage is recorded after its new RAN context has been created. */
    if (!ran && !strcmp(m.name, "InitialUEMessage")) return 0;
    char cause[96] = "";
    if (m.cause) {
        long value = 0;
        switch (m.cause->present) {
        case NGAP_Cause_PR_radioNetwork: value = m.cause->choice.radioNetwork; break;
        case NGAP_Cause_PR_transport: value = m.cause->choice.transport; break;
        case NGAP_Cause_PR_nas: value = m.cause->choice.nas; break;
        case NGAP_Cause_PR_protocol: value = m.cause->choice.protocol; break;
        case NGAP_Cause_PR_misc: value = m.cause->choice.misc; break;
        default: break;
        }
        snprintf(cause, sizeof(cause), "NGAP group=%d value=%ld", m.cause->present, value);
    }
    char info[256];
    char *p = ogs_slprintf(info, info + sizeof(info), "gNB=%u/%u peer=%s",
            gnb->gnb_id, gnb->gnb_id_bits, ogs_sockaddr_to_string_static(gnb->sctp.addr));
    if (m.has_amf)
        p = ogs_slprintf(p, info + sizeof(info), " AMF-UE-ID=%" PRIu64, m.amf_id);
    if (m.has_ran)
        ogs_slprintf(p, info + sizeof(info), " RAN-UE-ID=%" PRIu64, m.ran_id);
    uint64_t seq = record_event(ran, NULL, "NGAP", direction, m.name, result, cause, info, false, gnb, &m);
    if (ran && !strcmp(direction, "gNB->AMF")) ran->event_ngap_seq = seq;
    if (ran && strstr(m.name, "Handover") && strstr(m.name, "Failure"))
        amf_ue_event(ran, NULL, "ACTION", direction, "Handover",
                !strcmp(direction, "gNB->AMF") ? "peer-failed" : "failure-queued", cause, info, true);
    if (ran && !strcmp(m.name, "PathSwitchRequestAcknowledge"))
        amf_ue_event(ran, NULL, "ACTION", direction, "PathSwitch", result, cause, info, true);
    return seq;
}

uint64_t amf_ue_event_nas(ran_ue_t *ran, amf_ue_t *ue,
        ogs_nas_5gs_message_t *message, bool received)
{
    if (!ogs_ue_events_enabled() || !message) return 0;
    const char *name = "Unknown5GMM", *action = NULL, *result = NULL;
    ngap_meta_t wire = {0};
    char cause[96] = "";
    switch (message->gmm.h.message_type) {
#define MSG(k, n) case OGS_NAS_5GS_##k: name = n; break
    MSG(REGISTRATION_REQUEST, "RegistrationRequest");
    MSG(REGISTRATION_ACCEPT, "RegistrationAccept");
    MSG(REGISTRATION_COMPLETE, "RegistrationComplete");
    MSG(AUTHENTICATION_REQUEST, "AuthenticationRequest");
    MSG(AUTHENTICATION_RESPONSE, "AuthenticationResponse");
    MSG(AUTHENTICATION_RESULT, "AuthenticationResult");
    MSG(IDENTITY_REQUEST, "IdentityRequest");
    MSG(IDENTITY_RESPONSE, "IdentityResponse");
    MSG(SECURITY_MODE_COMMAND, "SecurityModeCommand");
    MSG(SECURITY_MODE_COMPLETE, "SecurityModeComplete");
    MSG(SERVICE_REQUEST, "ServiceRequest");
    case OGS_NAS_5GS_SERVICE_ACCEPT: name = "ServiceAccept"; action = "ServiceRequest"; result = "accept-prepared"; break;
    MSG(DEREGISTRATION_REQUEST_FROM_UE, "DeregistrationRequestFromUE");
    MSG(DEREGISTRATION_ACCEPT_FROM_UE, "DeregistrationAcceptFromUE");
    MSG(DEREGISTRATION_REQUEST_TO_UE, "DeregistrationRequestToUE");
    MSG(DEREGISTRATION_ACCEPT_TO_UE, "DeregistrationAcceptToUE");
    MSG(CONFIGURATION_UPDATE_COMMAND, "ConfigurationUpdateCommand");
    MSG(CONFIGURATION_UPDATE_COMPLETE, "ConfigurationUpdateComplete");
    case OGS_NAS_5GS_UL_NAS_TRANSPORT:
        name = "ULNASTransport";
        if (message->gmm.ul_nas_transport.presencemask & OGS_NAS_5GS_UL_NAS_TRANSPORT_PDU_SESSION_ID_PRESENT)
            ngap_psi(&wire, message->gmm.ul_nas_transport.pdu_session_id);
        break;
    case OGS_NAS_5GS_DL_NAS_TRANSPORT:
        name = "DLNASTransport";
        if (message->gmm.dl_nas_transport.presencemask & OGS_NAS_5GS_DL_NAS_TRANSPORT_PDU_SESSION_ID_PRESENT)
            ngap_psi(&wire, message->gmm.dl_nas_transport.pdu_session_id);
        break;
    case OGS_NAS_5GS_REGISTRATION_REJECT:
        name = "RegistrationReject"; action = "Registration"; result = "rejected";
        snprintf(cause, sizeof(cause), "5GMM cause=%u", message->gmm.registration_reject.gmm_cause); break;
    case OGS_NAS_5GS_SERVICE_REJECT:
        name = "ServiceReject"; action = "ServiceRequest"; result = "rejected";
        snprintf(cause, sizeof(cause), "5GMM cause=%u", message->gmm.service_reject.gmm_cause); break;
    case OGS_NAS_5GS_AUTHENTICATION_FAILURE:
        name = "AuthenticationFailure";
        snprintf(cause, sizeof(cause), "5GMM cause=%u", message->gmm.authentication_failure.gmm_cause); break;
    case OGS_NAS_5GS_AUTHENTICATION_REJECT: name = "AuthenticationReject"; break;
    case OGS_NAS_5GS_SECURITY_MODE_REJECT:
        name = "SecurityModeReject";
        snprintf(cause, sizeof(cause), "5GMM cause=%u", message->gmm.security_mode_reject.gmm_cause); break;
    case OGS_NAS_5GS_5GMM_STATUS: name = "5GMMStatus"; break;
    default: break;
#undef MSG
    }
    if (received && ue && (message->gmm.h.message_type == OGS_NAS_5GS_REGISTRATION_REQUEST ||
                message->gmm.h.message_type == OGS_NAS_5GS_SERVICE_REQUEST))
        ue->event_flow_id = ogs_ue_events_new_trace();
    uint64_t seq = record_event(ran, ue, "NAS-5GMM", received ? "UE->AMF" : "AMF->UE", name,
            received ? "received" : "prepared", cause, NULL, false, NULL, &wire);
    if (received && message->gmm.h.message_type == OGS_NAS_5GS_REGISTRATION_REQUEST)
        amf_ue_event(ran, ue, "ACTION", "UE->AMF", "Registration", "started", NULL, NULL, true);
    if (received && message->gmm.h.message_type == OGS_NAS_5GS_SERVICE_REQUEST)
        amf_ue_event(ran, ue, "ACTION", "UE->AMF", "ServiceRequest", "started", NULL, NULL, true);
    if (!received && action)
        amf_ue_event(ran, ue, "ACTION", "AMF->UE", action, result, cause,
                "NAS response prepared; see NGAP transmit result", true);
    return seq;
}

ogs_pkbuf_t *amf_ue_event_nas_encode(amf_ue_t *ue, ogs_nas_5gs_message_t *message)
{
    uint64_t seq = amf_ue_event_nas(NULL, ue, message, false);
    ogs_pkbuf_t *p = ogs_nas_5gs_plain_encode(message);
    if (p) ogs_ue_trace_record(seq, OGS_TRACE_5GMM, OGS_TRACE_PLAIN | OGS_TRACE_PREPARED, p->data, p->len);
    return p;
}
