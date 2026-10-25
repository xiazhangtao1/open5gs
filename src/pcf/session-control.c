/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "session-control.h"
#include "nsmf-build.h"

typedef struct {
    ogs_pool_id_t sess_id, stream_id;
    char instance[OGS_UUID_FORMATTED_LENGTH + 1];
    char transaction[OGS_UUID_FORMATTED_LENGTH + 1];
} update_context_t;

static bool reply(ogs_sbi_stream_t *stream, int status, cJSON *body)
{
    ogs_sbi_response_t *response = ogs_sbi_response_new();
    response->status = status;
    response->http.content = cJSON_PrintUnformatted(body);
    response->http.content_length = strlen(response->http.content);
    ogs_sbi_header_set(response->http.headers, OGS_SBI_CONTENT_TYPE, OGS_SBI_CONTENT_JSON_TYPE);
    return ogs_sbi_server_send_response(stream, response);
}

void pcf_session_control_initial(pcf_sess_t *sess, OpenAPI_session_rule_t *rule)
{
    ogs_sbi_session_config_t *c = &sess->session_config;
    OpenAPI_ambr_t *ambr = rule->auth_sess_ambr ? rule->auth_sess_ambr : sess->subscribed_sess_ambr;
    OpenAPI_arp_t *arp = NULL;
    if (ambr) {
        if (ambr->uplink) c->ambr.uplink = ogs_sbi_bitrate_from_string(ambr->uplink);
        if (ambr->downlink) c->ambr.downlink = ogs_sbi_bitrate_from_string(ambr->downlink);
    }
    if (rule->auth_def_qos) {
        OpenAPI_authorized_default_qos_t *q = rule->auth_def_qos;
        c->qos.index = q->_5qi;
        c->qos.priority_level = q->is_priority_level ? q->priority_level : 0;
        arp = q->arp;
    } else if (sess->subscribed_default_qos) {
        OpenAPI_subscribed_default_qos_t *q = sess->subscribed_default_qos;
        c->qos.index = q->_5qi;
        c->qos.priority_level = q->is_priority_level ? q->priority_level : 0;
        arp = q->arp;
    }
    if (arp) {
        c->qos.arp.priority_level = arp->priority_level;
        c->qos.arp.pre_emption_capability = arp->preempt_cap == OpenAPI_preemption_capability_MAY_PREEMPT ? OGS_5GC_PRE_EMPTION_ENABLED : OGS_5GC_PRE_EMPTION_DISABLED;
        c->qos.arp.pre_emption_vulnerability = arp->preempt_vuln == OpenAPI_preemption_vulnerability_PREEMPTABLE ? OGS_5GC_PRE_EMPTION_ENABLED : OGS_5GC_PRE_EMPTION_DISABLED;
    }
    sess->session_config_ready = c->qos.index && c->qos.arp.priority_level && c->ambr.uplink && c->ambr.downlink;
    ogs_cpystrn(sess->control_instance, sess->control_version, sizeof(sess->control_instance));
}

static void expire(pcf_sess_t *sess)
{
    ogs_sbi_session_operation_t *op = &sess->session_operation;
    if (op->state == OGS_SESSION_CONTROL_PENDING &&
        ogs_time_now() - op->updated_at >= ogs_time_from_sec(60)) {
        op->state = OGS_SESSION_CONTROL_UNKNOWN;
        if (op->pfcp == OGS_SESSION_CONTROL_PENDING) op->pfcp = OGS_SESSION_CONTROL_UNKNOWN;
        if (op->nas == OGS_SESSION_CONTROL_PENDING) op->nas = OGS_SESSION_CONTROL_UNKNOWN;
        if (op->ngap == OGS_SESSION_CONTROL_PENDING) op->ngap = OGS_SESSION_CONTROL_UNKNOWN;
        op->updated_at = ogs_time_now();
        ogs_cpystrn(op->detail, "No final SMF confirmation; inspect signaling before releasing the session", sizeof(op->detail));
        pcf_sess_control_touch(sess);
    }
}

void pcf_session_control_json(pcf_sess_t *sess, cJSON *body)
{
    expire(sess);
    cJSON_AddStringToObject(body, "controlInstance", sess->control_instance);
    if (sess->session_config_ready) {
        cJSON_AddItemToObject(body, "sessionConfig", ogs_sbi_session_config_json(&sess->session_config));
        cJSON_AddStringToObject(body, "sessionConfigSource", sess->session_config_confirmed ? "smf-confirmed" : "initial-policy");
    }
    if (sess->session_operation.state)
        cJSON_AddItemToObject(body, "sessionControl", ogs_sbi_session_operation_json(&sess->session_operation));
}

bool pcf_session_control_busy(pcf_sess_t *sess, bool release)
{
    expire(sess);
    return sess->session_operation.state == OGS_SESSION_CONTROL_PENDING ||
        (!release && sess->session_operation.state == OGS_SESSION_CONTROL_UNKNOWN);
}

static int update_callback(int status, ogs_sbi_response_t *response, void *data)
{
    update_context_t *ctx = data;
    pcf_sess_t *sess = pcf_sess_find_by_id(ctx->sess_id);
    ogs_sbi_stream_t *stream = ogs_sbi_stream_find_by_id(ctx->stream_id);
    int code = status == OGS_OK && response ? response->status : 504;
    bool same = sess && !strcmp(sess->control_instance, ctx->instance) &&
        !strcmp(sess->session_operation.transaction, ctx->transaction);
    if (same && code != 204 && sess->session_operation.state == OGS_SESSION_CONTROL_PENDING) {
        ogs_sbi_session_operation_t *op = &sess->session_operation;
        op->state = code >= 500 && code != 501 ? OGS_SESSION_CONTROL_UNKNOWN : OGS_SESSION_CONTROL_FAILED;
        op->pfcp = op->nas = op->ngap = op->state == OGS_SESSION_CONTROL_UNKNOWN ? OGS_SESSION_CONTROL_UNKNOWN : OGS_SESSION_CONTROL_NONE;
        op->updated_at = ogs_time_now();
        ogs_cpystrn(op->detail, "SMF did not confirm acceptance of the update", sizeof(op->detail));
        if (response && response->http.content && response->http.content_length < 4096) {
            cJSON *problem = cJSON_Parse(response->http.content);
            const char *detail = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(problem, "detail"));
            if (detail) ogs_cpystrn(op->detail, detail, sizeof(op->detail));
            cJSON_Delete(problem);
        }
        pcf_sess_control_touch(sess);
    }
    if (stream) {
        if (same && (code == 204 || sess->session_operation.state == OGS_SESSION_CONTROL_CONFIRMED)) {
            cJSON *body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "status", "accepted");
            cJSON_AddStringToObject(body, "operationId", sess->session_operation.operation_id);
            cJSON_AddStringToObject(body, "controlVersion", sess->control_version);
            reply(stream, 202, body); cJSON_Delete(body);
        } else ogs_sbi_server_send_error(stream,
                same ? (code >= 400 && code <= 599 ? code : 502) : 409,
                NULL, "Session update was not accepted or target changed", same ? sess->session_operation.detail : NULL, NULL);
    }
    if (response) ogs_sbi_response_free(response);
    ogs_free(ctx);
    return OGS_OK;
}

bool pcf_session_control_update(pcf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request)
{
    ogs_sbi_session_config_t target;
    ogs_sbi_session_operation_t *op = &sess->session_operation;
    const char *id = ogs_sbi_header_get(request->http.headers, "x-operation-id");
    cJSON *body = NULL, *root = NULL, *rules, *rule, *qos, *arp, *meta;
    OpenAPI_sm_policy_decision_t *decision = NULL;
    ogs_sbi_request_t *notification = NULL;
    update_context_t *ctx = NULL;
    ogs_uuid_t uuid;
    bool sent;
    if (!sess->session_config_ready || !sess->notification_uri || !sess->nsmf.client)
        return ogs_sbi_server_send_error(stream, 409, message, "Session policy not ready", NULL, NULL);
    body = request->http.content && request->http.content_length <= 4096 ? cJSON_ParseWithOpts(request->http.content, NULL, true) : NULL;
    if (!ogs_sbi_session_config_parse(body, &target) || (id && !ogs_sbi_session_control_id(id, false))) {
        cJSON_Delete(body);
        return ogs_sbi_server_send_error(stream, 400, message,
                "Invalid default QoS or Session-AMBR; use a supported Non-GBR 5QI and exactly representable positive rates", NULL, NULL);
    }
    if (ogs_sbi_session_config_equal(&target, &sess->session_config)) {
        cJSON *result = cJSON_CreateObject();
        cJSON_AddStringToObject(result, "status", "unchanged");
        sent = reply(stream, 200, result); cJSON_Delete(result); cJSON_Delete(body);
        return sent;
    }
    memset(op, 0, sizeof(*op));
    op->previous = sess->session_config; op->target = target;
    ogs_cpystrn(op->instance, sess->control_instance, sizeof(op->instance));
    ogs_uuid_get(&uuid); ogs_uuid_format(op->transaction, &uuid);
    ogs_cpystrn(op->operation_id, id ? id : op->transaction, sizeof(op->operation_id));
    op->state = op->pfcp = op->ngap = OGS_SESSION_CONTROL_PENDING;
    if (target.qos.index != op->previous.qos.index ||
        target.ambr.uplink != op->previous.ambr.uplink || target.ambr.downlink != op->previous.ambr.downlink)
        op->nas = OGS_SESSION_CONTROL_PENDING;
    op->updated_at = ogs_time_now();
    ogs_cpystrn(op->detail, "Awaiting SMF procedure confirmation", sizeof(op->detail));

    root = cJSON_CreateObject(); rules = cJSON_AddObjectToObject(root, "sessRules");
    rule = cJSON_AddObjectToObject(rules, "1");
    cJSON_AddStringToObject(rule, "sessRuleId", "1");
    qos = cJSON_DetachItemFromObjectCaseSensitive(body, "defaultQos");
    arp = cJSON_GetObjectItemCaseSensitive(qos, "arp");
    cJSON_AddItemToObject(arp, "preemptCap", cJSON_DetachItemFromObjectCaseSensitive(arp, "preemptionCapability"));
    cJSON_AddItemToObject(arp, "preemptVuln", cJSON_DetachItemFromObjectCaseSensitive(arp, "preemptionVulnerability"));
    cJSON_AddItemToObject(rule, "authDefQos", qos);
    cJSON_AddItemToObject(rule, "authSessAmbr", cJSON_DetachItemFromObjectCaseSensitive(body, "sessionAmbr"));
    decision = OpenAPI_sm_policy_decision_parseFromJSON(root);
    if (decision) notification = pcf_nsmf_callback_build_smpolicycontrol_update(sess, decision);
    cJSON_Delete(root); cJSON_Delete(body);
    if (decision) OpenAPI_sm_policy_decision_free(decision);
    if (!notification) goto failed;
    root = cJSON_Parse(notification->http.content);
    if (!root) goto failed;
    meta = cJSON_AddObjectToObject(root, "xcnControl");
    cJSON_AddStringToObject(meta, "instance", op->instance);
    cJSON_AddStringToObject(meta, "transaction", op->transaction);
    cJSON_AddStringToObject(meta, "operationId", op->operation_id);
    cJSON_AddItemToObject(meta, "previous", ogs_sbi_session_config_json(&op->previous));
    ogs_free(notification->http.content);
    notification->http.content = cJSON_PrintUnformatted(root);
    notification->http.content_length = strlen(notification->http.content);
    cJSON_Delete(root); root = NULL;
    notification->no_proxy = true;
    ctx = ogs_calloc(1, sizeof(*ctx)); ogs_assert(ctx);
    ctx->sess_id = sess->id; ctx->stream_id = ogs_sbi_id_from_stream(stream);
    ogs_cpystrn(ctx->instance, op->instance, sizeof(ctx->instance));
    ogs_cpystrn(ctx->transaction, op->transaction, sizeof(ctx->transaction));
    pcf_sess_control_touch(sess);
    sent = ogs_sbi_send_request_to_client(sess->nsmf.client, update_callback, notification, ctx);
    ogs_sbi_request_free(notification);
    if (sent) return true;
    notification = NULL; ogs_free(ctx);
failed:
    if (notification) ogs_sbi_request_free(notification);
    op->state = OGS_SESSION_CONTROL_FAILED;
    op->pfcp = op->nas = op->ngap = OGS_SESSION_CONTROL_NONE;
    op->updated_at = ogs_time_now();
    ogs_cpystrn(op->detail, "Update was not submitted", sizeof(op->detail));
    pcf_sess_control_touch(sess);
    return ogs_sbi_server_send_error(stream, 502, message, "Cannot submit session policy update", NULL, NULL);
}

bool pcf_session_control_result(pcf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, ogs_sbi_request_t *request)
{
    ogs_sbi_session_operation_t result = sess->session_operation;
    cJSON *body = request->http.content && request->http.content_length <= 4096 ? cJSON_ParseWithOpts(request->http.content, NULL, true) : NULL;
    const char *instance = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "instance"));
    const char *transaction = message->h.resource.component[4];
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "operationId"));
    bool valid = instance && transaction && id &&
        !strcmp(instance, sess->control_instance) && !strcmp(transaction, result.transaction) &&
        !strcmp(id, result.operation_id) && ogs_sbi_session_operation_result(body, &result) &&
        (sess->session_operation.nas != OGS_SESSION_CONTROL_PENDING || result.nas != OGS_SESSION_CONTROL_NONE);
    cJSON_Delete(body);
    if (!valid) return ogs_sbi_server_send_error(stream, 409, message, "Stale or invalid session-control result", NULL, NULL);
    if (sess->session_operation.state == OGS_SESSION_CONTROL_CONFIRMED || sess->session_operation.state == OGS_SESSION_CONTROL_FAILED) {
        if (result.state != sess->session_operation.state)
            return ogs_sbi_server_send_error(stream, 409, message, "Operation already settled", NULL, NULL);
        return ogs_sbi_send_http_status_no_content(stream);
    }
    sess->session_operation = result;
    if (result.state == OGS_SESSION_CONTROL_CONFIRMED) {
        sess->session_config = result.target;
        sess->session_config_confirmed = true;
    }
    pcf_sess_control_touch(sess);
    return ogs_sbi_send_http_status_no_content(stream);
}
