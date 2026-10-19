/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "user-intent.h"
#include "binding.h"
#include "gsm-build.h"
#include "gsm-handler.h"
#include "ngap-build.h"
#include "npcf-handler.h"
#include "pfcp-path.h"
#include "sbi-path.h"

#define INTENT_URI "/xcn-dedicated-bearer/v1/intent-operations"
#define INTENT_QUEUE_MAX 8
#define INTENT_COMMAND_INTERVAL ogs_time_from_sec(8)
#define INTENT_COMMAND_RETRIES 4

typedef struct {
    ogs_lnode_t node;
    ogs_pkbuf_t *nas;
    ogs_pool_id_t stream_id;
    uint8_t pti;
    smf_intent_value_t value;
} intent_request_t;

typedef struct {
    ogs_pool_id_t sess_id;
    uint64_t generation;
    int kind, status;
} intent_callback_t;

struct smf_user_intent_s {
    ogs_pool_id_t sess_id;
    uint64_t generation;
    ogs_list_t queue;
    intent_request_t *request;
    ogs_timer_t *timer;
    char transaction[OGS_UUID_FORMATTED_LENGTH + 1];
    char *app_id, *rule_id;
    bool exists, active, binding, uncertain, prepared;
    bool command_sent, complete, n2_done, pfcp_done, finalizing, success;
    uint8_t model, five_qi, priority, qfi;
    smf_intent_plan_t plan;
    ogs_qos_t previous_qos;
    ogs_pkbuf_t *command, *n2;
    unsigned command_retries;
    uint8_t epco[OGS_MAX_PCO_LEN];
    size_t epco_length;
};
typedef struct smf_user_intent_s intent_state_t;
static uint64_t next_generation;
static void start_next(smf_sess_t *sess);
static void finish(smf_sess_t *sess, bool success, bool uncertain);
static void settle(smf_sess_t *sess);

int smf_intent_parse(const uint8_t *data, size_t length, uint16_t id,
        const ogs_plmn_id_t *plmn, smf_intent_value_t *value)
{
    ogs_pco_t pco;
    int i, found = -1;
    size_t offset = 1;
    if (!data || !length || length > UINT16_MAX || !plmn || !value) {
        ogs_warn("Intent ePCO rejected: invalid input [length:%zu]", length);
        return OGS_ERROR;
    }
    /* Bounded diagnostic pass also shows the available bytes of a malformed
     * target container; never trust the advertised length for a hexdump. */
    while (offset + 3 <= length) {
        size_t count = data[offset + 2];
        uint16_t container = ((uint16_t)data[offset] << 8) | data[offset + 1];
        size_t available = length - offset - 3;
        if (container == id) {
            ogs_info("ePCO intent container 0x%04x length:%zu available:%zu "
                    "header:0x%02x ePCO-length:%zu",
                    id, count, available, data[0], length);
            ogs_log_hexdump(OGS_LOG_INFO, data + offset,
                    3 + ogs_min(count, available));
        }
        if (count > available) break;
        offset += 3 + count;
    }
    if (ogs_pco_parse(&pco, (uint8_t *)data, length) != (int)length) {
        ogs_warn("Intent ePCO rejected: malformed configuration items [length:%zu]", length);
        return OGS_ERROR;
    }
    for (i = 0; i < pco.num_of_id; i++) {
        if (pco.ids[i].id != id) continue;
        if (found >= 0) {
            ogs_warn("Intent ePCO rejected: duplicate container [id:0x%04x]", id);
            return OGS_ERROR;
        }
        found = i;
    }
    if (found < 0) return OGS_DONE;
    if (data[0] != 0x80) {
        ogs_warn("Intent ePCO rejected: invalid header [received:0x%02x expected:0x80]", data[0]);
        return OGS_ERROR;
    }
    if (pco.ids[found].len != 6) {
        ogs_warn("Intent ePCO rejected: invalid container length [received:%u expected:6]",
                pco.ids[found].len);
        return OGS_ERROR;
    }
    if (memcmp(pco.ids[found].data, plmn, 3)) {
        ogs_warn("Intent ePCO rejected: PLMN mismatch [received:0x%06x expected:0x%06x]",
                ogs_plmn_id_hexdump(pco.ids[found].data), ogs_plmn_id_hexdump(plmn));
        return OGS_ERROR;
    }
    data = (const uint8_t *)pco.ids[found].data + 3;
    if (data[0] > 1 || data[1] < 1 || data[1] > 3 ||
        data[2] < 1 || data[2] > 3) {
        ogs_warn("Intent ePCO rejected: invalid business values [commStatus:%u aiModel:%u gpuLevel:%u]",
                data[0], data[1], data[2]);
        return OGS_ERROR;
    }
    value->comm_status = data[0];
    value->ai_model = data[1];
    value->gpu_level = data[2];
    return OGS_OK;
}

smf_intent_plan_t smf_intent_plan(const smf_intent_value_t *v,
        bool exists, uint8_t model, uint8_t five_qi, uint8_t priority,
        const smf_intent_qos_config_t *profile)
{
    smf_intent_plan_t p = {SMF_INTENT_NONE, five_qi, priority};
    if (!v->comm_status) {
        if (exists) p.operation = SMF_INTENT_DELETE;
        return p;
    }
    if (v->ai_model == 1) return p;
    p.five_qi = profile->five_qi;
    p.priority = profile->priority_level;
    if (!exists) p.operation = SMF_INTENT_CREATE;
    else {
        p.operation = SMF_INTENT_MODIFY;
        if (model == v->ai_model && five_qi == p.five_qi) {
            p.priority = priority > 1 ? priority - 1 : 1;
            if (priority == 1) p.operation = SMF_INTENT_NONE;
        }
    }
    return p;
}

cJSON *smf_intent_notification(const smf_intent_value_t *v,
        const char *ip, const char *supi, bool success, uint8_t five_qi)
{
    cJSON *body;
    char raw[7];
    if (!ip || !*ip || !supi) return NULL;
    body = cJSON_CreateObject();
    ogs_snprintf(raw, sizeof(raw), "%02x%02x%02x",
            v->comm_status, v->ai_model, v->gpu_level);
    cJSON_AddStringToObject(body, "ueIp", ip);
    cJSON_AddStringToObject(body, "supi", supi);
    cJSON_AddNumberToObject(body, "commStatus", v->comm_status);
    cJSON_AddNumberToObject(body, "aiModel", v->ai_model);
    cJSON_AddNumberToObject(body, "gpuLevel", v->gpu_level);
    cJSON_AddStringToObject(body, "qosResult", success ? "SUCCESS" : "FAILURE");
    if (success && v->comm_status && five_qi)
        cJSON_AddNumberToObject(body, "fiveQi", five_qi);
    cJSON_AddStringToObject(body, "rawPco", raw);
    cJSON_AddNumberToObject(body, "timestamp", ogs_time_now() / 1000);
    return body;
}

static bool decimal(const char *s, size_t min, size_t max)
{
    size_t i, n = s ? strlen(s) : 0;
    if (n < min || n > max) return false;
    for (i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

int smf_user_intent_qos_config_parse(ogs_yaml_iter_t *parent,
        smf_intent_qos_config_t *qos)
{
    ogs_yaml_iter_t it;
    const char *profile = ogs_yaml_iter_key(parent);
    ogs_yaml_iter_recurse(parent, &it);
    if (ogs_yaml_iter_type(&it) != YAML_MAPPING_NODE) {
        ogs_error("user_intent.%s must be a mapping", profile);
        return OGS_ERROR;
    }
    while (ogs_yaml_iter_next(&it)) {
        const char *key = ogs_yaml_iter_key(&it), *value;
        if (!strcmp(key, "flow_descriptions")) {
            ogs_yaml_iter_t flows;
            const char *descriptions[OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE];
            int count = 0;
            ogs_yaml_iter_recurse(&it, &flows);
            if (ogs_yaml_iter_type(&flows) != YAML_SEQUENCE_NODE) {
                ogs_error("user_intent.%s.flow_descriptions must be a sequence", profile);
                return OGS_ERROR;
            }
            while (ogs_yaml_iter_next(&flows)) {
                ogs_flow_t parsed = {0};
                if (count == OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE ||
                    !ogs_yaml_iter_has_value(&flows) ||
                    ogs_flow_description_parse(&parsed, ogs_yaml_iter_value(&flows)) != OGS_OK) {
                    ogs_error("Invalid user_intent.%s.flow_descriptions (1..15 valid filters required)", profile);
                    return OGS_ERROR;
                }
                OGS_FLOW_FREE(&parsed);
                descriptions[count++] = ogs_yaml_iter_value(&flows);
            }
            if (!count) {
                ogs_error("user_intent.%s.flow_descriptions cannot be empty", profile);
                return OGS_ERROR;
            }
            memcpy(qos->flow_descriptions, descriptions, count * sizeof(descriptions[0]));
            qos->num_of_flow = count;
            continue;
        }
        if (!ogs_yaml_iter_has_value(&it)) {
            ogs_error("user_intent.%s.%s must be a scalar", profile, key);
            return OGS_ERROR;
        }
        value = ogs_yaml_iter_value(&it);
        if (!strcmp(key, "five_qi")) {
            if (!decimal(value, 1, 3) || atoi(value) < 1 || atoi(value) > 255) {
                ogs_error("user_intent.%s.five_qi must be 1..255", profile);
                return OGS_ERROR;
            }
            qos->five_qi = atoi(value);
        } else if (!strcmp(key, "priority_level")) {
            if (!decimal(value, 1, 3) || atoi(value) < 1 || atoi(value) > 127) {
                ogs_error("user_intent.%s.priority_level must be 1..127", profile);
                return OGS_ERROR;
            }
            qos->priority_level = atoi(value);
        } else if (!strcmp(key, "maxbr_dl")) qos->maxbr_dl = value;
        else if (!strcmp(key, "maxbr_ul")) qos->maxbr_ul = value;
        else if (!strcmp(key, "gbr_dl")) qos->gbr_dl = value;
        else if (!strcmp(key, "gbr_ul")) qos->gbr_ul = value;
        else ogs_warn("Unknown user_intent.%s option [%s]", profile, key);
    }
    return OGS_OK;
}

static bool intent_bitrate(const char *value, uint64_t *bitrate)
{
    char *unit;
    double number;
    uint64_t multiplier = 0;
    if (!value || (!(value[0] >= '0' && value[0] <= '9') && value[0] != '.'))
        return false;
    errno = 0;
    number = strtod(value, &unit);
    if (errno == ERANGE || unit == value || *unit != ' ') return false;
    if (!strcmp(unit + 1, "bps")) multiplier = 1;
    else if (!strcmp(unit + 1, "Kbps")) multiplier = 1000ULL;
    else if (!strcmp(unit + 1, "Mbps")) multiplier = 1000000ULL;
    else if (!strcmp(unit + 1, "Gbps")) multiplier = 1000000000ULL;
    else if (!strcmp(unit + 1, "Tbps")) multiplier = 1000000000000ULL;
    if (!multiplier || !(number > 0) ||
        !(number <= (double)OGS_MAX_BITRATE_NGAP / multiplier) ||
        number * multiplier < 1) return false;
    *bitrate = (uint64_t)(number * multiplier);
    return true;
}

static bool intent_qos_valid(const smf_intent_qos_config_t *qos,
        const char *profile)
{
    uint64_t max_dl, max_ul, gbr_dl, gbr_ul;
    int i;
    if (qos->num_of_flow < 1 || qos->num_of_flow > OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE) {
        ogs_error("user_intent.%s.flow_descriptions must contain 1..15 filters", profile);
        return false;
    }
    for (i = 0; i < qos->num_of_flow; i++) {
        ogs_flow_t parsed = {0};
        if (ogs_flow_description_parse(&parsed, qos->flow_descriptions[i]) != OGS_OK) {
            ogs_error("Invalid user_intent.%s.flow_descriptions[%d]", profile, i);
            return false;
        }
        OGS_FLOW_FREE(&parsed);
    }
    if (qos->five_qi < 1 || qos->five_qi > 255 ||
        qos->priority_level < 1 || qos->priority_level > 127 ||
        !intent_bitrate(qos->maxbr_dl, &max_dl) ||
        !intent_bitrate(qos->maxbr_ul, &max_ul) ||
        !intent_bitrate(qos->gbr_dl, &gbr_dl) ||
        !intent_bitrate(qos->gbr_ul, &gbr_ul) ||
        gbr_dl > max_dl || gbr_ul > max_ul) {
        ogs_error("Invalid user_intent.%s QoS: five_qi=1..255, priority_level=1..127, "
                "positive bitrate with bps/Kbps/Mbps/Gbps/Tbps unit, "
                "GBR <= MBR <= 4 Tbps required", profile);
        return false;
    }
    return true;
}

bool smf_intent_flows_match(const ogs_pcc_rule_t *rule,
        const smf_intent_qos_config_t *profile)
{
    int i, j;
    bool matched[OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE] = {false};
    if (!rule || !profile || profile->num_of_flow < 1 ||
        profile->num_of_flow > OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE ||
        rule->num_of_flow != profile->num_of_flow) return false;
    for (i = 0; i < profile->num_of_flow; i++) {
        ogs_flow_t parsed = {0};
        bool found = false;
        if (ogs_flow_description_parse(&parsed, profile->flow_descriptions[i]) != OGS_OK)
            return false;
        for (j = 0; j < rule->num_of_flow; j++) {
            if (!matched[j] && rule->flow[j].description &&
                rule->flow[j].direction == parsed.direction &&
                !strcmp(rule->flow[j].description, parsed.description)) {
                found = true;
                matched[j] = true;
                break;
            }
        }
        OGS_FLOW_FREE(&parsed);
        if (!found) return false;
    }
    return true;
}

static bool intent_filters_unchanged(smf_bearer_t *bearer,
        const smf_intent_qos_config_t *profile)
{
    ogs_pcc_rule_t filters = {0};
    smf_pf_t *pf;
    ogs_list_for_each(&bearer->pf_list, pf) {
        if (filters.num_of_flow == OGS_MAX_NUM_OF_FLOW_IN_PCC_RULE) return false;
        filters.flow[filters.num_of_flow].direction = pf->direction;
        filters.flow[filters.num_of_flow++].description = pf->flow_description;
    }
    /* Descriptions are borrowed from the active bearer, not owned by filters. */
    return smf_intent_flows_match(&filters, profile);
}

static bool intent_qos_unchanged(const ogs_qos_t *qos,
        const smf_intent_qos_config_t *profile)
{
    uint64_t max_dl, max_ul, gbr_dl, gbr_ul;
    return qos->index == profile->five_qi &&
        qos->priority_level == profile->priority_level &&
        qos->arp.priority_level == 8 &&
        qos->arp.pre_emption_capability == OGS_5GC_PRE_EMPTION_DISABLED &&
        qos->arp.pre_emption_vulnerability == OGS_5GC_PRE_EMPTION_ENABLED &&
        intent_bitrate(profile->maxbr_dl, &max_dl) && qos->mbr.downlink == max_dl &&
        intent_bitrate(profile->maxbr_ul, &max_ul) && qos->mbr.uplink == max_ul &&
        intent_bitrate(profile->gbr_dl, &gbr_dl) && qos->gbr.downlink == gbr_dl &&
        intent_bitrate(profile->gbr_ul, &gbr_ul) && qos->gbr.uplink == gbr_ul;
}

int smf_user_intent_config_validate(void)
{
    smf_context_t *c = smf_self();
    if (!c->user_intent.enabled) return OGS_OK;
    if (!intent_qos_valid(&c->user_intent.face_recognition, "face_recognition") ||
        !intent_qos_valid(&c->user_intent.object_recognition, "object_recognition"))
        return OGS_ERROR;
    if (!decimal(c->user_intent.mcc, 3, 3) ||
        !decimal(c->user_intent.mnc, 2, 3) ||
        !c->user_intent.notify_uri ||
        strncmp(c->user_intent.notify_uri, "http://", 7)) {
        ogs_error("user_intent requires explicit MCC, 2/3-digit MNC and http:// notify_uri");
        return OGS_ERROR;
    }
    ogs_plmn_id_build(&c->user_intent.plmn, atoi(c->user_intent.mcc),
            atoi(c->user_intent.mnc), strlen(c->user_intent.mnc));
    return OGS_OK;
}

static void free_request(intent_request_t *r)
{
    if (!r) return;
    ogs_pkbuf_free(r->nas);
    ogs_free(r);
}

static int http_callback(int status, ogs_sbi_response_t *response, void *data)
{
    intent_callback_t *cb = data;
    smf_event_t *e = smf_event_new(SMF_EVT_USER_INTENT);
    cb->status = status;
    e->h.sbi.data = cb;
    e->h.sbi.response = response;
    if (ogs_queue_push(ogs_app()->queue, e) != OGS_OK) {
        if (response) ogs_sbi_response_free(response);
        ogs_free(cb);
        ogs_event_free(e);
        return OGS_ERROR;
    }
    ogs_pollset_notify(ogs_app()->pollset);
    return OGS_OK;
}

static void timeout_callback(void *data)
{
    intent_state_t *st = data;
    intent_callback_t *cb = ogs_calloc(1, sizeof(*cb));
    cb->sess_id = st->sess_id;
    cb->generation = st->generation;
    cb->kind = 3;
    http_callback(OGS_TIMEUP, NULL, cb);
}

static bool send_json(smf_sess_t *sess, ogs_sbi_client_t *client,
        const char *method, const char *uri, cJSON *body, int kind,
        ogs_time_t deadline)
{
    intent_state_t *st = sess->user_intent;
    intent_callback_t *cb = ogs_calloc(1, sizeof(*cb));
    ogs_sbi_request_t *req = ogs_sbi_request_new();
    bool sent;
    cb->sess_id = sess->id;
    cb->generation = st->generation;
    cb->kind = kind;
    req->h.method = ogs_strdup(method);
    req->h.uri = ogs_strdup(uri);
    req->http.content = cJSON_PrintUnformatted(body);
    req->http.content_length = strlen(req->http.content);
    req->deadline = deadline;
    req->no_proxy = kind == 4;
    ogs_sbi_header_set(req->http.headers, "Content-Type", "application/json; charset=utf-8");
    sent = ogs_sbi_client_send_request(client, http_callback, req, cb);
    ogs_sbi_request_free(req);
    if (!sent) ogs_free(cb);
    return sent;
}

static bool pcf_request(smf_sess_t *sess, cJSON *body, bool finalize)
{
    ogs_sbi_client_t *client = sess->policy_association.client;
    ogs_sbi_header_t h = {0};
    char *uri;
    bool sent;
    if (!client) return false;
    h.service.name = (char *)"xcn-dedicated-bearer";
    h.api.version = (char *)"v1";
    h.resource.component[0] = (char *)"intent-operations";
    uri = ogs_sbi_client_uri(client, &h);
    sent = send_json(sess, client, finalize ? "PATCH" : "POST", uri, body,
            finalize ? 2 : 1, 0);
    ogs_free(uri);
    return sent;
}

static cJSON *base_body(smf_sess_t *sess)
{
    intent_state_t *st = sess->user_intent;
    smf_ue_t *ue = smf_ue_find_by_id(sess->smf_ue_id);
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "supi", ue->supi);
    cJSON_AddNumberToObject(body, "pduSessionId", sess->psi);
    cJSON_AddStringToObject(body, "transaction", st->transaction);
    if (st->app_id) cJSON_AddStringToObject(body, "appSessionId", st->app_id);
    return body;
}

static void notify(smf_sess_t *sess)
{
    intent_state_t *st = sess->user_intent;
    smf_ue_t *ue = smf_ue_find_by_id(sess->smf_ue_id);
    ogs_sbi_client_t *client;
    OpenAPI_uri_scheme_e scheme;
    char *fqdn = NULL, ip[INET_ADDRSTRLEN];
    uint16_t port;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;
    cJSON *body;
    const char *uri = smf_self()->user_intent.notify_uri;
    if (!sess->ipv4 || !inet_ntop(AF_INET, &sess->ipv4->addr, ip, sizeof(ip))) {
        ogs_error("Intent [%s] notification unavailable: no UE IPv4 address", st->transaction);
        return;
    }
    body = smf_intent_notification(&st->request->value, ip, ue->supi,
            st->success, st->exists ? st->five_qi : 0);
    if (!ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &port, &addr, &addr6, (char *)uri)) {
        ogs_error("Intent [%s] invalid notification URI", st->transaction);
        cJSON_Delete(body);
        return;
    }
    client = ogs_sbi_client_find(scheme, fqdn, port, addr, addr6);
    if (!client) client = ogs_sbi_client_add(scheme, fqdn, port, addr, addr6);
    ogs_info("Intent [%s] notify [%s:%u] qosResult:%s", st->transaction,
            ue->supi, sess->psi, st->success ? "SUCCESS" : "FAILURE");
    if (!client || !send_json(sess, client, "POST", uri, body, 4, ogs_time_from_msec(1000)))
        ogs_error("Intent [%s] notification send failed (no retry)", st->transaction);
    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);
    cJSON_Delete(body);
}

static void reject(smf_sess_t *sess, ogs_sbi_stream_t *stream, uint8_t pti, uint8_t cause)
{
    uint8_t old = sess->pti;
    ogs_pkbuf_t *nas;
    sess->pti = pti;
    nas = gsm_build_pdu_session_modification_reject(sess, cause);
    sess->pti = old;
    if (nas) smf_sbi_send_sm_context_update_error_n1_n2_message(stream,
            OGS_SBI_HTTP_STATUS_BAD_REQUEST, nas, OpenAPI_n2_sm_info_type_NULL, NULL);
}

/* Process each standard container separately: legacy PCO response storage is
 * bounded, and PAP/CHAP/IPCP must not read past the supplied container. */
int smf_intent_epco_response(uint8_t *result, size_t capacity,
        const uint8_t *data, size_t data_length)
{
    ogs_pco_t pco;
    uint8_t input[259], output[OGS_MAX_PCO_LEN];
    int i, length;
    size_t result_length = 1;
    if (!result || !capacity || !data || !data_length || data_length > UINT16_MAX)
        return 0;
    result[0] = 0x80;
    if (ogs_pco_parse(&pco, (uint8_t *)data, data_length) != (int)data_length)
        return 0;
    for (i = 0; i < pco.num_of_id; i++) {
        ogs_pco_id_t *id = &pco.ids[i];
        const uint8_t *data = id->data;
        size_t offset;
        if (id->id == smf_self()->user_intent.container_id) continue;
        if (id->id == OGS_PCO_ID_PASSWORD_AUTHENTICATION_PROTOCOL ||
            id->id == OGS_PCO_ID_CHALLENGE_HANDSHAKE_AUTHENTICATION_PROTOCOL ||
            id->id == OGS_PCO_ID_INTERNET_PROTOCOL_CONTROL_PROTOCOL) {
            if (id->len < 4 || (((unsigned)data[2] << 8) | data[3]) != id->len)
                return 0;
            if (id->id == OGS_PCO_ID_INTERNET_PROTOCOL_CONTROL_PROTOCOL) {
                for (offset = 4; offset < id->len; offset += data[offset + 1])
                    if (id->len - offset < 2 || data[offset + 1] < 2 ||
                        data[offset + 1] > id->len - offset) return 0;
            }
        }
        input[0] = 0x80;
        input[1] = id->id >> 8; input[2] = id->id & 0xff; input[3] = id->len;
        if (id->len) memcpy(input + 4, id->data, id->len);
        length = smf_pco_build(output, input, 4 + id->len);
        if (length < 1 || result_length + length - 1 > capacity)
            return 0;
        memcpy(result + result_length, output + 1, length - 1);
        result_length += length - 1;
    }
    return result_length;
}

static void send_command(smf_sess_t *sess, ogs_sbi_stream_t *stream)
{
    intent_state_t *st = sess->user_intent;
    if (!st->command_sent && st->epco_length > 1) {
        ogs_nas_5gs_message_t message;
        ogs_pkbuf_t *encoded;
        if (ogs_nas_5gsm_decode(&message, st->command) != OGS_OK) {
            finish(sess, false, st->prepared); return;
        }
        message.gsm.pdu_session_modification_command.presencemask |=
            OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND_EXTENDED_PROTOCOL_CONFIGURATION_OPTIONS_PRESENT;
        message.gsm.pdu_session_modification_command.extended_protocol_configuration_options.buffer = st->epco;
        message.gsm.pdu_session_modification_command.extended_protocol_configuration_options.length = st->epco_length;
        encoded = ogs_nas_5gs_plain_encode(&message);
        if (!encoded) { finish(sess, false, st->prepared); return; }
        ogs_pkbuf_free(st->command);
        st->command = encoded;
    }
    smf_sbi_send_sm_context_updated_data_n1_n2_message(sess, stream,
            ogs_pkbuf_copy(st->command),
            st->n2 ? OpenAPI_n2_sm_info_type_PDU_RES_MOD_REQ : OpenAPI_n2_sm_info_type_NULL,
            st->n2 ? ogs_pkbuf_copy(st->n2) : NULL);
    if (!st->command_sent) ogs_timer_start(st->timer, INTENT_COMMAND_INTERVAL);
    st->command_sent = true;
}

bool smf_user_intent_busy(smf_sess_t *sess)
{
    return sess->user_intent && sess->user_intent->active;
}
void smf_user_intent_resume(smf_sess_t *sess)
{
    start_next(sess);
}
uint64_t smf_user_intent_generation(smf_sess_t *sess)
{
    return sess->user_intent && sess->user_intent->active ?
        sess->user_intent->generation : 0;
}
bool smf_user_intent_binding(smf_sess_t *sess)
{
    return sess->user_intent && sess->user_intent->binding;
}

static bool same_request(intent_request_t *r, ogs_pkbuf_t *nas)
{
    return r && r->nas->len == nas->len && !memcmp(r->nas->data, nas->data, nas->len);
}

bool smf_user_intent_request(smf_sess_t *sess, ogs_sbi_stream_t *stream,
        ogs_nas_5gs_pdu_session_modification_request_t *req)
{
    smf_intent_value_t value;
    ogs_nas_5gs_message_t message = {0};
    ogs_pkbuf_t *nas;
    intent_state_t *st = sess->user_intent;
    intent_request_t *r;
    smf_ue_t *ue;
    const char *supi;
    int parsed;
    if (!smf_self()->user_intent.enabled || !(req->presencemask &
            OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REQUEST_EXTENDED_PROTOCOL_CONFIGURATION_OPTIONS_PRESENT))
        return false;
    ue = smf_ue_find_by_id(sess->smf_ue_id);
    supi = ue && ue->supi ? ue->supi : "unknown";
    ogs_info("[%s:%u] ePCO modification request [PTI:%u length:%u]",
            supi, sess->psi, sess->pti,
            req->extended_protocol_configuration_options.length);
    parsed = smf_intent_parse(req->extended_protocol_configuration_options.buffer,
            req->extended_protocol_configuration_options.length,
            smf_self()->user_intent.container_id, &smf_self()->user_intent.plmn, &value);
    if (parsed == OGS_DONE) return false;
    if (parsed != OGS_OK || !sess->pti || sess->pti == 255) {
        uint8_t cause = parsed != OGS_OK ?
            OGS_5GSM_CAUSE_INVALID_MANDATORY_INFORMATION : OGS_5GSM_CAUSE_INVALID_PTI_VALUE;
        ogs_warn("[%s:%u] Intent request rejected [PTI:%u cause:%u]",
                supi, sess->psi, sess->pti, cause);
        reject(sess, stream, sess->pti, cause);
        return true;
    }
    ogs_info("[%s:%u] Intent validated [PTI:%u commStatus:%u aiModel:%u gpuLevel:%u]",
            supi, sess->psi, sess->pti, value.comm_status, value.ai_model, value.gpu_level);
    if (!st) {
        st = sess->user_intent = ogs_calloc(1, sizeof(*st));
        st->sess_id = sess->id;
        ogs_list_init(&st->queue);
        st->timer = ogs_timer_add(ogs_app()->timer_mgr, timeout_callback, st);
    }
    message.gsm.h.extended_protocol_discriminator = OGS_NAS_EXTENDED_PROTOCOL_DISCRIMINATOR_5GSM;
    message.gsm.h.pdu_session_identity = sess->psi;
    message.gsm.h.procedure_transaction_identity = sess->pti;
    message.gsm.h.message_type = OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REQUEST;
    message.gsm.pdu_session_modification_request = *req;
    nas = ogs_nas_5gs_plain_encode(&message);
    if (!nas) {
        reject(sess, stream, sess->pti, OGS_5GSM_CAUSE_INVALID_MANDATORY_INFORMATION);
        return true;
    }
    if (st->active && st->request->pti == sess->pti) {
        if (!same_request(st->request, nas))
            reject(sess, stream, sess->pti, OGS_5GSM_CAUSE_PTI_ALREADY_IN_USE);
        else if (st->command_sent) send_command(sess, stream);
        else {
            ogs_sbi_stream_t *old = ogs_sbi_stream_find_by_id(st->request->stream_id);
            if (old && old != stream) ogs_sbi_send_http_status_no_content(old);
            st->request->stream_id = ogs_sbi_id_from_stream(stream);
        }
        ogs_pkbuf_free(nas);
        return true;
    }
    ogs_list_for_each(&st->queue, r) {
        if (r->pti != sess->pti) continue;
        if (!same_request(r, nas)) reject(sess, stream, sess->pti,
                OGS_5GSM_CAUSE_PTI_ALREADY_IN_USE);
        else {
            ogs_sbi_stream_t *old = ogs_sbi_stream_find_by_id(r->stream_id);
            if (old && old != stream) ogs_sbi_send_http_status_no_content(old);
            r->stream_id = ogs_sbi_id_from_stream(stream);
        }
        ogs_pkbuf_free(nas);
        return true;
    }
    if (ogs_list_count(&st->queue) >= INTENT_QUEUE_MAX) {
        reject(sess, stream, sess->pti, OGS_5GSM_CAUSE_INSUFFICIENT_RESOURCES);
        ogs_pkbuf_free(nas);
        return true;
    }
    r = ogs_calloc(1, sizeof(*r));
    r->nas = nas;
    r->pti = sess->pti;
    r->stream_id = ogs_sbi_id_from_stream(stream);
    r->value = value;
    ogs_list_add(&st->queue, r);
    start_next(sess);
    return true;
}

static void start_next(smf_sess_t *sess)
{
    intent_state_t *st = sess->user_intent;
    ogs_sbi_stream_t *stream;
    smf_bearer_t *bearer;
    const smf_intent_qos_config_t *profile;
    ogs_uuid_t uuid;
    cJSON *body, *qos, *arp;
    ogs_nas_5gs_message_t decoded;
    if (!st || st->active || sess->sm_policy_update_in_progress) return;
    st->request = ogs_list_first(&st->queue);
    if (!st->request) return;
    ogs_list_remove(&st->queue, st->request);
    st->active = true;
    st->generation = ++next_generation;
    st->command_sent = st->complete = st->n2_done = st->pfcp_done = false;
    st->prepared = st->finalizing = st->success = false;
    st->command_retries = 0;
    ogs_uuid_get(&uuid);
    ogs_uuid_format(st->transaction, &uuid);
    sess->pti = st->request->pti;
    /* Bound the asynchronous preparation wait separately from T3591 and
     * the notification client deadline. */
    ogs_timer_start(st->timer, ogs_time_from_sec(30));
    stream = ogs_sbi_stream_find_by_id(st->request->stream_id);
    if (!stream) { finish(sess, false, false); return; }
    if (st->uncertain) { finish(sess, false, true); return; }
    bearer = st->rule_id ? smf_qos_flow_find_by_pcc_rule_id(sess, st->rule_id) : NULL;
    if (st->exists && !bearer) { finish(sess, false, true); return; }
    if (bearer) {
        st->five_qi = bearer->qos.index;
        st->priority = bearer->qos.priority_level;
        st->previous_qos = bearer->qos;
        st->qfi = bearer->qfi;
    }
    profile = st->request->value.ai_model == 2 ?
        &smf_self()->user_intent.face_recognition :
        &smf_self()->user_intent.object_recognition;
    st->plan = smf_intent_plan(&st->request->value, st->exists,
            st->model, st->five_qi, st->priority, profile);
    /* A model switch may select an identical profile. Complete it as a
     * NAS-only transaction rather than asking binding to apply an empty delta. */
    if (bearer && st->plan.operation == SMF_INTENT_MODIFY &&
        st->model != st->request->value.ai_model &&
        intent_qos_unchanged(&bearer->qos, profile) &&
        intent_filters_unchanged(bearer, profile))
        st->plan.operation = SMF_INTENT_NONE;
    if (ogs_nas_5gsm_decode(&decoded, st->request->nas) != OGS_OK) {
        finish(sess, false, false); return;
    }
    st->epco_length = smf_intent_epco_response(st->epco, sizeof(st->epco),
            decoded.gsm.pdu_session_modification_request.extended_protocol_configuration_options.buffer,
            decoded.gsm.pdu_session_modification_request.extended_protocol_configuration_options.length);
    if (!st->epco_length) {
        finish(sess, false, false); return;
    }
    /* Combining mutations to arbitrary UE-selected rules with an application
     * policy delta is ambiguous. Reject the entire transaction before either
     * mutates state, rather than silently dropping standard QoS IEs. */
    if (decoded.gsm.pdu_session_modification_request.presencemask &
        (OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REQUEST_REQUESTED_QOS_RULES_PRESENT |
         OGS_NAS_5GS_PDU_SESSION_MODIFICATION_REQUEST_REQUESTED_QOS_FLOW_DESCRIPTIONS_PRESENT)) {
        finish(sess, false, false); return;
    }
    if (st->plan.operation == SMF_INTENT_NONE) {
        st->command = gsm_build_pdu_session_modification_command(sess, 0, 0);
        if (!st->command) { finish(sess, false, false); return; }
        st->pfcp_done = st->n2_done = true;
        send_command(sess, stream);
        return;
    }
    body = base_body(sess);
    cJSON_AddStringToObject(body, "operation", st->plan.operation == SMF_INTENT_CREATE ?
            "create" : st->plan.operation == SMF_INTENT_MODIFY ? "modify" : "delete");
    cJSON_AddStringToObject(body, "mediaType", "audio");
    if (st->plan.operation != SMF_INTENT_DELETE) {
        const char *max_dl = profile->maxbr_dl, *max_ul = profile->maxbr_ul;
        const char *gbr_dl = profile->gbr_dl, *gbr_ul = profile->gbr_ul;
        char bandwidth[4][32];
        if (st->plan.operation == SMF_INTENT_CREATE ||
            st->model != st->request->value.ai_model) {
            cJSON *flows = cJSON_AddArrayToObject(body, "flowDescriptions");
            int i;
            for (i = 0; i < profile->num_of_flow; i++)
                cJSON_AddItemToArray(flows, cJSON_CreateString(profile->flow_descriptions[i]));
        }
        /* Same-model requests change only priority, preserving effective QoS
         * even if another authorized policy has adjusted the bandwidth. */
        if (bearer && st->model == st->request->value.ai_model &&
            st->five_qi == st->plan.five_qi) {
            ogs_snprintf(bandwidth[0], sizeof(bandwidth[0]), "%" PRIu64 " bps",
                    bearer->qos.mbr.downlink);
            ogs_snprintf(bandwidth[1], sizeof(bandwidth[1]), "%" PRIu64 " bps",
                    bearer->qos.mbr.uplink);
            ogs_snprintf(bandwidth[2], sizeof(bandwidth[2]), "%" PRIu64 " bps",
                    bearer->qos.gbr.downlink);
            ogs_snprintf(bandwidth[3], sizeof(bandwidth[3]), "%" PRIu64 " bps",
                    bearer->qos.gbr.uplink);
            max_dl = bandwidth[0]; max_ul = bandwidth[1];
            gbr_dl = bandwidth[2]; gbr_ul = bandwidth[3];
        }
        qos = cJSON_AddObjectToObject(body, "qos");
        cJSON_AddNumberToObject(qos, "5qi", st->plan.five_qi);
        cJSON_AddNumberToObject(qos, "priorityLevel", st->plan.priority);
        cJSON_AddStringToObject(qos, "maxbrDl", max_dl);
        cJSON_AddStringToObject(qos, "maxbrUl", max_ul);
        cJSON_AddStringToObject(qos, "gbrDl", gbr_dl);
        cJSON_AddStringToObject(qos, "gbrUl", gbr_ul);
        arp = cJSON_AddObjectToObject(qos, "arp");
        cJSON_AddNumberToObject(arp, "priorityLevel", 8);
        cJSON_AddStringToObject(arp, "preemptionCapability", "NOT_PREEMPT");
        cJSON_AddStringToObject(arp, "preemptionVulnerability", "PREEMPTABLE");
    }
    if (!pcf_request(sess, body, false)) finish(sess, false, false);
    cJSON_Delete(body);
}

static void settle(smf_sess_t *sess)
{
    intent_state_t *st = sess->user_intent;
    smf_bearer_t *bearer;
    if (st->success) {
        st->exists = st->plan.operation != SMF_INTENT_DELETE &&
                (st->exists || st->plan.operation == SMF_INTENT_CREATE);
        if (st->exists) {
            st->five_qi = st->plan.five_qi;
            st->priority = st->plan.priority;
            /* LLM keeps the resource's last successfully applied model. */
            if (st->request->value.comm_status && st->request->value.ai_model != 1)
                st->model = st->request->value.ai_model;
        } else if (st->plan.operation == SMF_INTENT_DELETE) {
            st->model = 0;
            bearer = smf_qos_flow_find_by_pcc_rule_id(sess, st->rule_id);
            if (bearer) smf_bearer_remove(bearer);
            ogs_free(st->app_id); st->app_id = NULL;
            ogs_free(st->rule_id); st->rule_id = NULL;
        }
    }
    notify(sess);
    ogs_timer_stop(st->timer);
    free_request(st->request); st->request = NULL;
    if (st->command) ogs_pkbuf_free(st->command);
    if (st->n2) ogs_pkbuf_free(st->n2);
    st->command = st->n2 = NULL;
    st->active = st->binding = false;
    start_next(sess);
    if (!st->active)
        smf_npcf_smpolicycontrol_resume_updates(sess);
}

static void finish(smf_sess_t *sess, bool success, bool uncertain)
{
    intent_state_t *st = sess->user_intent;
    ogs_sbi_stream_t *stream;
    cJSON *body;
    if (!st || !st->active || st->finalizing) return;
    st->success = success;
    st->uncertain |= uncertain;
    st->finalizing = true;
    ogs_timer_start(st->timer, ogs_time_from_sec(30));
    stream = ogs_sbi_stream_find_by_id(st->request->stream_id);
    if (!st->command_sent && stream) {
        reject(sess, stream, st->request->pti, OGS_5GSM_CAUSE_REQUEST_REJECTED_UNSPECIFIED);
    }
    if (st->prepared) {
        body = base_body(sess);
        cJSON_AddStringToObject(body, "outcome", success ? "commit" :
                st->uncertain ? "uncertain" : "abort");
        if (pcf_request(sess, body, true)) {
            cJSON_Delete(body);
            return;
        }
        cJSON_Delete(body);
        st->success = false;
        st->uncertain = true;
    }
    settle(sess);
}

void smf_user_intent_complete(smf_sess_t *sess, uint8_t pti)
{
    intent_state_t *st = sess->user_intent;
    if (!st || !st->active || !st->command_sent || st->request->pti != pti) return;
    st->complete = true;
    ogs_timer_start(st->timer, ogs_time_from_sec(30));
    if (st->pfcp_done && st->n2_done) finish(sess, true, false);
}

void smf_user_intent_rejected(smf_sess_t *sess, uint8_t pti)
{
    intent_state_t *st = sess->user_intent;
    if (st && st->active && st->request->pti == pti)
        finish(sess, false, st->prepared);
}

void smf_user_intent_pfcp(smf_sess_t *sess, uint64_t flags, bool success)
{
    intent_state_t *st = sess->user_intent;
    ogs_sbi_stream_t *stream;
    smf_bearer_t *bearer;
    if (!st || !st->active || st->finalizing) return;
    if (!success) { finish(sess, false, true); return; }
    if (flags & OGS_PFCP_MODIFY_ACTIVATE) {
        st->pfcp_done = true;
        if (st->complete && st->n2_done) finish(sess, true, false);
        return;
    }
    stream = ogs_sbi_stream_find_by_id(st->request->stream_id);
    if (!stream) { finish(sess, false, true); return; }
    bearer = smf_qos_flow_find_by_pcc_rule_id(sess, st->rule_id);
    if (!bearer) { finish(sess, false, true); return; }
    st->qfi = bearer->qfi;
    sess->pti = st->request->pti;
    st->command = gsm_build_pdu_session_modification_command(sess,
            QOS_RULE_CODE_FROM_PFCP_FLAGS(flags), QOS_RULE_FLOW_DESCRIPTION_CODE_FROM_PFCP_FLAGS(flags));
    st->n2 = st->plan.operation == SMF_INTENT_DELETE ?
        ngap_build_pdu_session_resource_release_request_transfer(sess,
                NGAP_Cause_PR_nas, NGAP_CauseNas_normal_release) :
        ngap_build_pdu_session_resource_modify_request_transfer(sess, true);
    if (!st->command || !st->n2) { finish(sess, false, true); return; }
    if (st->plan.operation == SMF_INTENT_DELETE) st->pfcp_done = true;
    send_command(sess, stream);
}

bool smf_user_intent_n2(smf_sess_t *sess, ogs_sbi_stream_t *stream, ogs_pkbuf_t *pkbuf)
{
    intent_state_t *st = sess->user_intent;
    NGAP_PDUSessionResourceModifyResponseTransfer_t transfer = {0};
    smf_bearer_t *bearer;
    bool accepted = false;
    int i, rv;
    if (!st || !st->active || !st->command_sent || !st->n2 || st->finalizing) return false;
    rv = ogs_asn_decode(&asn_DEF_NGAP_PDUSessionResourceModifyResponseTransfer,
            &transfer, sizeof(transfer), pkbuf);
    if (rv == OGS_OK) {
        accepted = st->plan.operation == SMF_INTENT_DELETE;
        if (transfer.qosFlowAddOrModifyResponseList) {
            for (i = 0; i < transfer.qosFlowAddOrModifyResponseList->list.count; i++)
                if (transfer.qosFlowAddOrModifyResponseList->list.array[i]->qosFlowIdentifier == st->qfi)
                    accepted = true;
        }
        if (transfer.qosFlowFailedToAddOrModifyList) {
            for (i = 0; i < transfer.qosFlowFailedToAddOrModifyList->list.count; i++)
                if (transfer.qosFlowFailedToAddOrModifyList->list.array[i]->qosFlowIdentifier == st->qfi)
                    accepted = false;
        }
    }
    ogs_asn_free(&asn_DEF_NGAP_PDUSessionResourceModifyResponseTransfer, &transfer);
    ogs_sbi_send_http_status_no_content(stream);
    if (!accepted) { finish(sess, false, true); return true; }
    if (st->n2_done) return true;
    st->n2_done = true;
    if (st->plan.operation == SMF_INTENT_DELETE) {
        if (st->complete) finish(sess, true, false);
        return true;
    }
    bearer = smf_qos_flow_find_by_pcc_rule_id(sess, st->rule_id);
    if (!bearer) { finish(sess, false, true); return true; }
    bearer->dl_far->apply_action = OGS_PFCP_APPLY_ACTION_FORW;
    if (ogs_pfcp_ip_to_outer_header_creation(&sess->remote_dl_ip,
            &bearer->dl_far->outer_header_creation,
            &bearer->dl_far->outer_header_creation_len) != OGS_OK) {
        finish(sess, false, true); return true;
    }
    bearer->dl_far->outer_header_creation.teid = sess->remote_dl_teid;
    ogs_list_init(&sess->qos_flow_to_modify_list);
    ogs_list_add(&sess->qos_flow_to_modify_list, &bearer->to_modify_node);
    if (smf_5gc_pfcp_send_qos_flow_list_modification_request(sess, NULL,
            OGS_PFCP_MODIFY_USER_INTENT|OGS_PFCP_MODIFY_DL_ONLY|OGS_PFCP_MODIFY_ACTIVATE, 0) != OGS_OK)
        finish(sess, false, true);
    return true;
}

void smf_user_intent_event(smf_event_t *e)
{
    intent_callback_t *cb = e->h.sbi.data;
    ogs_sbi_response_t *response = e->h.sbi.response;
    smf_sess_t *sess = smf_sess_find_by_id(cb->sess_id);
    intent_state_t *st = sess ? sess->user_intent : NULL;
    cJSON *body = NULL, *item;
    OpenAPI_sm_policy_decision_t *decision = NULL;
    const char *app_id, *rule_id, *transaction;
    if (cb->kind == 4) {
        if (cb->status != OGS_OK || !response || response->status != 204)
            ogs_error("Intent notify [session:%u generation:%llu] failed [transport:%d HTTP:%d], no retry",
                    cb->sess_id, (unsigned long long)cb->generation, cb->status, response ? response->status : 0);
        goto done;
    }
    if (!st || !st->active || st->generation != cb->generation) goto done;
    if (cb->kind == 3) {
        if (!st->finalizing && st->command_sent && !st->complete &&
            st->command_retries < INTENT_COMMAND_RETRIES) {
            smf_n1_n2_message_transfer_param_t param = {0};
            param.state = SMF_NETWORK_REQUESTED_QOS_FLOW_MODIFICATION;
            param.n1smbuf = ogs_pkbuf_copy(st->command);
            /* N2 was already delivered; retransmit the same NAS command only. */
            smf_namf_comm_send_n1_n2_message_transfer(sess, NULL, &param);
            st->command_retries++;
            ogs_timer_start(st->timer, INTENT_COMMAND_INTERVAL);
            goto done;
        }
        if (st->finalizing) {
            st->success = false; st->uncertain = true; settle(sess);
        } else finish(sess, false, st->prepared);
        goto done;
    }
    if (cb->kind == 2) {
        if (cb->status != OGS_OK || !response || response->status != 204) {
            st->success = false;
            st->uncertain = true;
        }
        settle(sess);
        goto done;
    }
    if (st->finalizing) goto done;
    if (cb->status != OGS_OK || !response || response->status != 200 || !response->http.content) {
        finish(sess, false, cb->status != OGS_OK);
        goto done;
    }
    body = cJSON_Parse(response->http.content);
    item = cJSON_GetObjectItemCaseSensitive(body, "appSessionId");
    app_id = cJSON_IsString(item) ? item->valuestring : NULL;
    item = cJSON_GetObjectItemCaseSensitive(body, "pccRuleId");
    rule_id = cJSON_IsString(item) ? item->valuestring : NULL;
    item = cJSON_GetObjectItemCaseSensitive(body, "transaction");
    transaction = cJSON_IsString(item) ? item->valuestring : NULL;
    item = cJSON_GetObjectItemCaseSensitive(body, "decision");
    if (cJSON_IsObject(item)) decision = OpenAPI_sm_policy_decision_parseFromJSON(item);
    if (!app_id || !rule_id || !transaction || strcmp(transaction, st->transaction) || !decision) {
        finish(sess, false, true); goto done;
    }
    if ((st->app_id && strcmp(st->app_id, app_id)) || (st->rule_id && strcmp(st->rule_id, rule_id))) {
        finish(sess, false, true); goto done;
    }
    if (!st->app_id) st->app_id = ogs_strdup(app_id);
    if (!st->rule_id) st->rule_id = ogs_strdup(rule_id);
    st->prepared = true;
    st->binding = true;
    if (!smf_npcf_apply_intent_decision(sess, decision)) finish(sess, false, true);
    st->binding = false;
done:
    if (decision) OpenAPI_sm_policy_decision_free(decision);
    cJSON_Delete(body);
    if (response) ogs_sbi_response_free(response);
    ogs_free(cb);
}

void smf_user_intent_clear(smf_sess_t *sess)
{
    intent_state_t *st = sess->user_intent;
    intent_request_t *r, *next;
    if (!st) return;
    if (st->timer) ogs_timer_delete(st->timer);
    free_request(st->request);
    ogs_list_for_each_safe(&st->queue, next, r) {
        ogs_list_remove(&st->queue, r);
        free_request(r);
    }
    if (st->command) ogs_pkbuf_free(st->command);
    if (st->n2) ogs_pkbuf_free(st->n2);
    ogs_free(st->app_id);
    ogs_free(st->rule_id);
    ogs_free(st);
    sess->user_intent = NULL;
}
