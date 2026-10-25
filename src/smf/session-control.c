/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "session-control.h"
#include "user-intent.h"
#include "pfcp-path.h"
#include "gsm-build.h"
#include "ngap-build.h"
#include "namf-build.h"
#include "npcf-handler.h"
#include "ue-events.h"

typedef struct {
    ogs_pool_id_t sess_id;
    uint64_t generation;
    int kind, status;
} control_callback_t;

struct smf_session_control_s {
    ogs_sbi_session_operation_t op;
    uint64_t generation;
    ogs_pool_id_t sess_id;
    uint8_t qfi;
    bool qos_changed, ambr_changed, command_sent;
    unsigned retransmissions;
    ogs_timer_t *deadline, *nas_timer;
    ogs_pkbuf_t *command;
};
typedef struct smf_session_control_s control_state_t;
static uint64_t next_generation;

static void finish(smf_sess_t *sess, int result, const char *detail);

bool smf_session_control_connected(const smf_sess_t *sess)
{
    smf_bearer_t *b;
    OpenAPI_up_cnx_state_e state;
    if (!sess || !sess->remote_dl_teid || sess->n1_released || sess->n2_released ||
        sess->resource_status == OpenAPI_resource_status_RELEASED) return false;
    state = sess->up_cnx_state ? sess->up_cnx_state : sess->nsmf_param.up_cnx_state;
    if (state != OpenAPI_up_cnx_state_NULL && state != OpenAPI_up_cnx_state_ACTIVATED)
        return false;
    b = smf_default_bearer_in_sess((smf_sess_t *)sess);
    /* Initial setup may omit upCnxState; successful N2/N4 activation leaves
     * the established N3 endpoint and the default downlink FAR forwarding. */
    return b && b->qfi && b->dl_far && (b->dl_far->apply_action & OGS_PFCP_APPLY_ACTION_FORW);
}

bool smf_session_control_busy(const smf_sess_t *sess)
{
    return sess && sess->session_control &&
        (sess->session_control->op.state == OGS_SESSION_CONTROL_PENDING ||
         sess->session_control->op.state == OGS_SESSION_CONTROL_UNKNOWN);
}

bool smf_session_control_pending(const smf_sess_t *sess)
{
    return sess && sess->session_control &&
        sess->session_control->op.state == OGS_SESSION_CONTROL_PENDING;
}

bool smf_session_control_qos_changed(const smf_sess_t *sess)
{
    return smf_session_control_pending(sess) && sess->session_control->qos_changed;
}

bool smf_session_control_ambr_changed(const smf_sess_t *sess)
{
    return smf_session_control_pending(sess) && sess->session_control->ambr_changed;
}

uint64_t smf_session_control_generation(const smf_sess_t *sess)
{
    return sess && sess->session_control ? sess->session_control->generation : 0;
}

static int callback(int status, ogs_sbi_response_t *response, void *data)
{
    control_callback_t *cb = data;
    smf_event_t *e = smf_event_new(SMF_EVT_SESSION_CONTROL);
    cb->status = status; e->h.sbi.data = cb; e->h.sbi.response = response;
    if (ogs_queue_push(ogs_app()->queue, e) != OGS_OK) {
        if (response) ogs_sbi_response_free(response);
        ogs_free(cb); ogs_event_free(e); return OGS_ERROR;
    }
    ogs_pollset_notify(ogs_app()->pollset);
    return OGS_OK;
}

static control_callback_t *new_callback(control_state_t *st, int kind)
{
    control_callback_t *cb = ogs_calloc(1, sizeof(*cb)); ogs_assert(cb);
    cb->sess_id = st->sess_id; cb->generation = st->generation; cb->kind = kind;
    return cb;
}

static void deadline_callback(void *data)
{
    callback(OGS_TIMEUP, NULL, new_callback(data, 1));
}

static void nas_callback(void *data)
{
    callback(OGS_TIMEUP, NULL, new_callback(data, 2));
}

static void report(smf_sess_t *sess)
{
    control_state_t *st = sess->session_control;
    smf_ue_t *ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_sbi_header_t h = {0};
    ogs_sbi_request_t *req;
    control_callback_t *cb;
    cJSON *body;
    char psi[4];
    bool sent;
    if (!sess->policy_association.client || !ue) return;
    h.service.name = (char *)"xcn-session-control";
    h.api.version = (char *)"v1";
    h.resource.component[0] = (char *)"sessions";
    h.resource.component[1] = ue->supi;
    ogs_snprintf(psi, sizeof(psi), "%u", sess->psi);
    h.resource.component[2] = psi;
    h.resource.component[3] = (char *)"operations";
    h.resource.component[4] = st->op.transaction;
    h.resource.component[5] = (char *)"result";
    req = ogs_sbi_request_new();
    req->h.method = ogs_strdup("POST");
    req->h.uri = ogs_sbi_client_uri(sess->policy_association.client, &h);
    body = ogs_sbi_session_operation_json(&st->op);
    req->http.content = cJSON_PrintUnformatted(body);
    req->http.content_length = strlen(req->http.content);
    req->deadline = ogs_time_from_sec(5);
    req->no_proxy = true;
    ogs_sbi_header_set(req->http.headers, OGS_SBI_CONTENT_TYPE, OGS_SBI_CONTENT_JSON_TYPE);
    cJSON_Delete(body);
    cb = new_callback(st, 3);
    /* This private management resource is not an NRF-discovered service.
     * Use the established PCF client directly, as for ePCO intent results. */
    sent = ogs_sbi_client_send_request(sess->policy_association.client, callback, req, cb);
    ogs_sbi_request_free(req);
    if (!sent) ogs_free(cb);
}

static bool send_command(smf_sess_t *sess, bool n2)
{
    control_state_t *st = sess->session_control;
    smf_n1_n2_message_transfer_param_t param = {0};
    ogs_sbi_request_t *request;
    control_callback_t *cb;
    bool sent;
    param.state = SMF_NETWORK_REQUESTED_QOS_FLOW_MODIFICATION;
    if (st->command) param.n1smbuf = ogs_pkbuf_copy(st->command);
    if (n2) param.n2smbuf = ngap_build_pdu_session_resource_modify_request_transfer(sess, false);
    if ((st->command && !param.n1smbuf) || (n2 && !param.n2smbuf)) {
        if (param.n1smbuf) ogs_pkbuf_free(param.n1smbuf);
        if (param.n2smbuf) ogs_pkbuf_free(param.n2smbuf);
        return false;
    }
    request = smf_namf_comm_build_n1_n2_message_transfer(sess, &param);
    if (!request) return false; /* Builder owns both packet buffers. */
    request->no_proxy = true;
    cb = new_callback(st, 4);
    /* The serving AMF's callback client identifies the same AMF instance.
     * Use the normal N1N2 builder and a generation-checked result callback. */
    sent = ogs_sbi_send_request_to_client(sess->namf.client, callback, request, cb);
    ogs_sbi_request_free(request);
    if (!sent) ogs_free(cb);
    return sent;
}

static void finish(smf_sess_t *sess, int result, const char *detail)
{
    control_state_t *st = sess->session_control;
    if (!st || st->op.state != OGS_SESSION_CONTROL_PENDING) return;
    st->op.state = result; st->op.updated_at = ogs_time_now();
    if (result == OGS_SESSION_CONTROL_UNKNOWN) {
        if (st->op.pfcp == OGS_SESSION_CONTROL_PENDING) st->op.pfcp = OGS_SESSION_CONTROL_UNKNOWN;
        if (st->op.nas == OGS_SESSION_CONTROL_PENDING) st->op.nas = OGS_SESSION_CONTROL_UNKNOWN;
        if (st->op.ngap == OGS_SESSION_CONTROL_PENDING) st->op.ngap = OGS_SESSION_CONTROL_UNKNOWN;
    } else if (result == OGS_SESSION_CONTROL_FAILED) {
        st->op.pfcp = OGS_SESSION_CONTROL_FAILED;
        st->op.nas = st->op.ngap = OGS_SESSION_CONTROL_NONE;
    }
    ogs_cpystrn(st->op.detail, detail, sizeof(st->op.detail));
    ogs_timer_stop(st->deadline); ogs_timer_stop(st->nas_timer);
    if (st->command) ogs_pkbuf_free(st->command);
    st->command = NULL;
    if (result == OGS_SESSION_CONTROL_FAILED) {
        smf_bearer_t *b = smf_default_bearer_in_sess(sess);
        sess->session.qos = st->op.previous.qos;
        sess->session.ambr = st->op.previous.ambr;
        if (b && b->qfi == st->qfi) {
            b->qos = st->op.previous.qos;
            if (b->qer) {
                b->qer->mbr.uplink = st->op.previous.ambr.uplink;
                b->qer->mbr.downlink = st->op.previous.ambr.downlink;
            }
        }
    }
    report(sess);
    smf_ue_event_qos(sess, result == OGS_SESSION_CONTROL_CONFIRMED ? "completed" :
            result == OGS_SESSION_CONTROL_FAILED ? "failed" : "uncertain",
            st->qfi, st->op.target.qos.index, detail);
    if (result != OGS_SESSION_CONTROL_UNKNOWN) {
        smf_user_intent_resume(sess);
        smf_npcf_smpolicycontrol_resume_updates(sess);
    }
}

static void maybe_complete(smf_sess_t *sess)
{
    control_state_t *st = sess->session_control;
    if (st->op.pfcp == OGS_SESSION_CONTROL_CONFIRMED &&
        st->op.ngap == OGS_SESSION_CONTROL_CONFIRMED &&
        (st->op.nas == OGS_SESSION_CONTROL_NONE || st->op.nas == OGS_SESSION_CONTROL_CONFIRMED))
        finish(sess, OGS_SESSION_CONTROL_CONFIRMED,
                "Required signaling replies received; rate enforcement is not verified");
}

bool smf_session_control_request(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request)
{
    OpenAPI_sm_policy_notification_t *notification = message->SmPolicyNotification;
    OpenAPI_session_rule_t *rule = NULL;
    OpenAPI_map_t *map;
    cJSON *body, *meta, *config, *qos, *arp;
    const char *instance, *transaction, *id;
    ogs_sbi_session_config_t target, expected, current = {0};
    smf_bearer_t *b;
    control_state_t *st;
    bool valid;
    if (!notification || !notification->sm_policy_decision ||
        !notification->sm_policy_decision->sess_rules) return false;
    if (request->http.content_length > 8192) {
        ogs_sbi_server_send_error(stream, 413, message, "Session policy too large", NULL, NULL);
        return true;
    }
    body = request->http.content ? cJSON_Parse(request->http.content) : NULL;
    meta = cJSON_GetObjectItemCaseSensitive(body, "xcnControl");
    instance = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(meta, "instance"));
    transaction = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(meta, "transaction"));
    id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(meta, "operationId"));
    if (notification->sm_policy_decision->sess_rules->count == 1) {
        map = notification->sm_policy_decision->sess_rules->first->data;
        if (map && map->key && !strcmp(map->key, "1")) rule = map->value;
    }
    config = cJSON_CreateObject();
    if (rule && rule->auth_def_qos && rule->auth_sess_ambr) {
        qos = OpenAPI_authorized_default_qos_convertToJSON(rule->auth_def_qos);
        arp = cJSON_GetObjectItemCaseSensitive(qos, "arp");
        cJSON_AddItemToObject(arp, "preemptionCapability", cJSON_DetachItemFromObjectCaseSensitive(arp, "preemptCap"));
        cJSON_AddItemToObject(arp, "preemptionVulnerability", cJSON_DetachItemFromObjectCaseSensitive(arp, "preemptVuln"));
        cJSON_AddItemToObject(config, "defaultQos", qos);
        cJSON_AddItemToObject(config, "sessionAmbr", OpenAPI_ambr_convertToJSON(rule->auth_sess_ambr));
    }
    valid = ogs_sbi_session_control_id(instance, true) &&
        ogs_sbi_session_control_id(transaction, true) && ogs_sbi_session_control_id(id, false) &&
        ogs_sbi_session_config_parse(config, &target) &&
        ogs_sbi_session_config_parse(cJSON_GetObjectItemCaseSensitive(meta, "previous"), &expected);
    cJSON_Delete(config);
    if (!valid) {
        cJSON_Delete(body);
        ogs_sbi_server_send_error(stream, 400, message, "Invalid or uncorrelated live session policy", NULL, NULL);
        return true;
    }
    current.qos = sess->session.qos; current.ambr = sess->session.ambr;
    b = smf_default_bearer_in_sess(sess);
    if (smf_session_control_busy(sess) || smf_user_intent_busy(sess) ||
        sess->sm_policy_update_in_progress || ogs_list_first(&sess->sm_policy_update_list) ||
        !OGS_FSM_CHECK(&sess->sm, smf_gsm_state_operational) ||
        !smf_session_control_connected(sess) || sess->handover.prepared ||
        !b || !b->qfi || !b->qer || !sess->namf.client ||
        !ogs_sbi_session_config_equal(&expected, &current)) {
        char detail[160];
        ogs_snprintf(detail, sizeof(detail),
                "operational=%d up=%d control=%d intent=%d policy=%d queued=%d handover=%d default=%d amf=%d configMatch=%d",
                OGS_FSM_CHECK(&sess->sm, smf_gsm_state_operational), sess->up_cnx_state,
                smf_session_control_busy(sess), smf_user_intent_busy(sess), sess->sm_policy_update_in_progress,
                ogs_list_first(&sess->sm_policy_update_list) != NULL, sess->handover.prepared,
                b && b->qfi && b->qer, sess->namf.client != NULL,
                ogs_sbi_session_config_equal(&expected, &current));
        cJSON_Delete(body);
        ogs_sbi_server_send_error(stream, 409, message,
                "Session must be connected, unchanged and free of pending procedures", detail, NULL);
        return true;
    }
    if (HOME_ROUTED_ROAMING_IN_HSMF(sess) || HOME_ROUTED_ROAMING_IN_VSMF(sess)) {
        cJSON_Delete(body);
        ogs_sbi_server_send_error(stream, 501, message, "Only local/LBO sessions are supported", NULL, NULL);
        return true;
    }
    smf_session_control_clear(sess);
    st = ogs_calloc(1, sizeof(*st)); ogs_assert(st);
    sess->session_control = st; st->sess_id = sess->id;
    st->generation = ++next_generation;
    st->op.previous = current; st->op.target = target; st->qfi = b->qfi;
    ogs_cpystrn(st->op.instance, instance, sizeof(st->op.instance));
    ogs_cpystrn(st->op.transaction, transaction, sizeof(st->op.transaction));
    ogs_cpystrn(st->op.operation_id, id, sizeof(st->op.operation_id));
    cJSON_Delete(body);
    st->qos_changed = !ogs_sbi_session_qos_equal(&target.qos, &current.qos);
    st->ambr_changed = target.ambr.uplink != current.ambr.uplink || target.ambr.downlink != current.ambr.downlink;
    st->op.state = st->op.pfcp = st->op.ngap = OGS_SESSION_CONTROL_PENDING;
    if (target.qos.index != current.qos.index || st->ambr_changed) st->op.nas = OGS_SESSION_CONTROL_PENDING;
    st->op.updated_at = ogs_time_now();
    st->deadline = ogs_timer_add(ogs_app()->timer_mgr, deadline_callback, st);
    st->nas_timer = ogs_timer_add(ogs_app()->timer_mgr, nas_callback, st);
    ogs_assert(st->deadline && st->nas_timer);
    ogs_timer_start(st->deadline, ogs_time_from_sec(60));
    /* Stage the parameters for existing builders; snapshots retain the last
     * confirmed configuration separately until all required replies arrive. */
    sess->session.qos = b->qos = target.qos;
    sess->session.ambr = target.ambr;
    b->qer->mbr.uplink = target.ambr.uplink;
    b->qer->mbr.downlink = target.ambr.downlink;
    ogs_list_init(&sess->qos_flow_to_modify_list);
    ogs_list_add(&sess->qos_flow_to_modify_list, &b->to_modify_node);
    ogs_sbi_send_http_status_no_content(stream);
    if (smf_5gc_pfcp_send_qos_flow_list_modification_request(sess, NULL,
            OGS_PFCP_MODIFY_SESSION_CONTROL|OGS_PFCP_MODIFY_QOS_MODIFY, 0) != OGS_OK)
        finish(sess, OGS_SESSION_CONTROL_FAILED, "PFCP update was not submitted");
    return true;
}

void smf_session_control_pfcp(smf_sess_t *sess, bool success)
{
    control_state_t *st = sess->session_control;
    if (!smf_session_control_pending(sess)) return;
    st->op.pfcp = success ? OGS_SESSION_CONTROL_CONFIRMED : OGS_SESSION_CONTROL_UNKNOWN;
    st->op.updated_at = ogs_time_now();
    if (!success) { finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "PFCP application not confirmed"); return; }
    if (st->op.nas == OGS_SESSION_CONTROL_PENDING) {
        st->command = gsm_build_pdu_session_modification_command(sess, 0,
                st->op.target.qos.index != st->op.previous.qos.index ? OGS_NAS_MODIFY_NEW_QOS_FLOW_DESCRIPTION : 0);
        if (!st->command) { finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "Cannot encode NAS modification"); return; }
    }
    st->command_sent = true;
    if (!send_command(sess, true)) {
        finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "Cannot submit N1/N2 transfer"); return;
    }
    if (st->command) ogs_timer_start(st->nas_timer, ogs_time_from_sec(8));
}

bool smf_session_control_nas(smf_sess_t *sess, uint8_t pti, bool success)
{
    control_state_t *st = sess->session_control;
    if (!smf_session_control_busy(sess) || pti != 0) return false;
    if (!smf_session_control_pending(sess) || !st->command_sent || !st->command) return true;
    st->op.nas = success ? OGS_SESSION_CONTROL_CONFIRMED : OGS_SESSION_CONTROL_FAILED;
    st->op.updated_at = ogs_time_now(); ogs_timer_stop(st->nas_timer);
    if (!success) finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "UE rejected the NAS modification; partial application possible");
    else maybe_complete(sess);
    return true;
}

bool smf_session_control_n2(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_pkbuf_t *pkbuf, bool success)
{
    control_state_t *st = sess->session_control;
    NGAP_PDUSessionResourceModifyResponseTransfer_t transfer = {0};
    bool accepted = false;
    int i, rv;
    if (!smf_session_control_busy(sess)) return false;
    if (!smf_session_control_pending(sess) || !st->command_sent) {
        ogs_sbi_send_http_status_no_content(stream); return true;
    }
    if (success) {
        rv = ogs_asn_decode(&asn_DEF_NGAP_PDUSessionResourceModifyResponseTransfer, &transfer, sizeof(transfer), pkbuf);
        if (rv == OGS_OK) {
            accepted = !st->qos_changed;
            if (transfer.qosFlowAddOrModifyResponseList)
                for (i = 0; i < transfer.qosFlowAddOrModifyResponseList->list.count; i++) {
                    NGAP_QosFlowAddOrModifyResponseItem_t *item = transfer.qosFlowAddOrModifyResponseList->list.array[i];
                    if (item && item->qosFlowIdentifier == st->qfi) accepted = true;
                }
            if (transfer.qosFlowFailedToAddOrModifyList && transfer.qosFlowFailedToAddOrModifyList->list.count)
                accepted = false;
        }
        ogs_asn_free(&asn_DEF_NGAP_PDUSessionResourceModifyResponseTransfer, &transfer);
    }
    ogs_sbi_send_http_status_no_content(stream);
    st->op.ngap = accepted ? OGS_SESSION_CONTROL_CONFIRMED : OGS_SESSION_CONTROL_FAILED;
    st->op.updated_at = ogs_time_now();
    if (!accepted) finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "RAN modification not confirmed; partial application possible");
    else maybe_complete(sess);
    return true;
}

void smf_session_control_event(smf_event_t *e)
{
    control_callback_t *cb = e->h.sbi.data;
    ogs_sbi_response_t *response = e->h.sbi.response;
    smf_sess_t *sess = smf_sess_find_by_id(cb->sess_id);
    control_state_t *st = sess ? sess->session_control : NULL;
    if (!st || st->generation != cb->generation) goto done;
    if (cb->kind == 3) {
        if (cb->status != OGS_OK || !response || response->status != 204)
            ogs_error("Session-control result report not acknowledged [transport:%d HTTP:%d]; no automatic retry",
                    cb->status, response ? response->status : 0);
        goto done;
    }
    if (!smf_session_control_pending(sess)) goto done;
    if (cb->kind == 1) {
        finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "Session modification deadline exceeded");
    } else if (cb->kind == 2 && st->op.nas == OGS_SESSION_CONTROL_PENDING && st->command) {
        if (st->retransmissions++ < 4 && send_command(sess, false))
            ogs_timer_start(st->nas_timer, ogs_time_from_sec(8));
        else finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "NAS modification completion not received");
    } else if (cb->kind == 4) {
        ogs_sbi_message_t message = {0};
        bool initiated = false;
        if (cb->status == OGS_OK && response && ogs_sbi_parse_response(&message, response) == OGS_OK) {
            initiated = message.res_status == 200 && message.N1N2MessageTransferRspData &&
                message.N1N2MessageTransferRspData->cause == OpenAPI_n1_n2_message_transfer_cause_N1_N2_TRANSFER_INITIATED;
            ogs_sbi_message_free(&message);
        }
        if (!initiated) finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "AMF did not confirm immediate N1/N2 transfer");
    }
done:
    if (response) ogs_sbi_response_free(response);
    ogs_free(cb);
}

cJSON *smf_session_control_snapshot(const smf_sess_t *sess)
{
    ogs_sbi_session_config_t current = {0};
    cJSON *body;
    current.qos = sess->session.qos; current.ambr = sess->session.ambr;
    body = cJSON_CreateObject();
    cJSON_AddItemToObject(body, "observed", ogs_sbi_session_config_json(&current));
    if (sess->session_control) {
        const control_state_t *st = sess->session_control;
        cJSON_AddItemToObject(body, "operation", ogs_sbi_session_operation_json(&st->op));
        cJSON_AddItemToObject(body, "confirmed", ogs_sbi_session_config_json(
                st->op.state == OGS_SESSION_CONTROL_CONFIRMED ? &st->op.target : &st->op.previous));
    } else cJSON_AddItemToObject(body, "confirmed", ogs_sbi_session_config_json(&current));
    return body;
}

void smf_session_control_cancel(smf_sess_t *sess)
{
    if (smf_session_control_pending(sess))
        finish(sess, OGS_SESSION_CONTROL_UNKNOWN, "Session is releasing or being removed");
}

void smf_session_control_clear(smf_sess_t *sess)
{
    control_state_t *st = sess->session_control;
    if (!st) return;
    ogs_timer_delete(st->deadline); ogs_timer_delete(st->nas_timer);
    if (st->command) ogs_pkbuf_free(st->command);
    ogs_free(st); sess->session_control = NULL;
}
