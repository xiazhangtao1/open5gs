/*
 * Copyright (C) 2019-2025 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "sbi-path.h"

#include "npcf-handler.h"

#include <errno.h>
#include <curl/curl.h>

#define XCN_SVC_DEDICATED_BEARER "xcn-dedicated-bearer"
#define XCN_SVC_CORE_QUERY "xcn-core-query"
#define XCN_RESOURCE_BEARERS "bearers"
#define XCN_RESOURCE_USERS "users"
#define XCN_RESOURCE_SESSIONS "sessions"
#define XCN_AMF_UE_INFO_URL "http://xcn-amf:9090/ue-info"

typedef struct xcn_http_buffer_s {
    char *data;
    size_t len;
} xcn_http_buffer_t;

static size_t xcn_http_write_cb(
        void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t realsize = size * nmemb;
    xcn_http_buffer_t *buffer = userp;
    char *data = NULL;

    ogs_assert(buffer);

    data = ogs_realloc(buffer->data, buffer->len + realsize + 1);
    if (!data)
        return 0;

    buffer->data = data;
    memcpy(&(buffer->data[buffer->len]), contents, realsize);
    buffer->len += realsize;
    buffer->data[buffer->len] = '\0';

    return realsize;
}

static char *xcn_http_get(const char *url)
{
    CURL *curl = NULL;
    CURLcode res;
    long status = 0;
    xcn_http_buffer_t buffer = {0};

    ogs_assert(url);

    curl = curl_easy_init();
    if (!curl) {
        ogs_error("curl_easy_init() failed");
        return NULL;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, xcn_http_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 500L);

    res = curl_easy_perform(curl);
    if (res == CURLE_OK)
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || status < 200 || status >= 300) {
        ogs_warn("GET %s failed [%s,status:%ld]",
                url, curl_easy_strerror(res), status);
        if (buffer.data)
            ogs_free(buffer.data);
        return NULL;
    }

    return buffer.data;
}

static bool xcn_send_json_response(
        ogs_sbi_stream_t *stream, int status, cJSON *item)
{
    char *content = NULL;
    ogs_sbi_response_t *response = NULL;

    ogs_assert(stream);
    ogs_assert(item);

    content = cJSON_PrintUnformatted(item);
    if (!content) {
        ogs_error("cJSON_PrintUnformatted() failed");
        return false;
    }

    response = ogs_sbi_response_new();
    if (!response) {
        ogs_error("ogs_sbi_response_new() failed");
        cJSON_free(content);
        return false;
    }

    response->status = status;
    response->http.content = ogs_strdup(content);
    ogs_assert(response->http.content);
    response->http.content_length = strlen(response->http.content);
    ogs_sbi_header_set(response->http.headers,
            OGS_SBI_CONTENT_TYPE, OGS_SBI_CONTENT_JSON_TYPE);

    cJSON_free(content);

    return ogs_sbi_server_send_response(stream, response);
}

static bool xcn_send_error(ogs_sbi_stream_t *stream,
        ogs_sbi_message_t *message, int status, const char *detail)
{
    ogs_assert(stream);

    return ogs_sbi_server_send_error(stream, status, message,
            detail ? detail : "XCN request failed", NULL, NULL);
}

static const char *xcn_json_string(cJSON *item, const char *key)
{
    cJSON *child = NULL;

    ogs_assert(item);
    ogs_assert(key);

    child = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!cJSON_IsString(child))
        return NULL;

    return child->valuestring;
}

static int xcn_json_int(cJSON *item, const char *key, int default_value)
{
    cJSON *child = NULL;

    ogs_assert(item);
    ogs_assert(key);

    child = cJSON_GetObjectItemCaseSensitive(item, key);
    if (!cJSON_IsNumber(child))
        return default_value;

    return child->valueint;
}

static uint64_t xcn_json_uint64(cJSON *item, const char *key)
{
    cJSON *child = NULL;

    ogs_assert(item);
    ogs_assert(key);

    child = cJSON_GetObjectItemCaseSensitive(item, key);
    if (cJSON_IsNumber(child) && child->valuedouble >= 0)
        return (uint64_t)child->valuedouble;
    if (cJSON_IsString(child) && child->valuestring)
        return strtoull(child->valuestring, NULL, 10);

    return 0;
}

static uint64_t xcn_string_uint64(const char *value)
{
    char *end = NULL;
    unsigned long long number = 0;

    if (!value || !*value)
        return 0;

    errno = 0;
    number = strtoull(value, &end, 10);
    if (errno || !end || *end != '\0')
        return 0;

    return (uint64_t)number;
}

static char *xcn_supi_from_amf_tmsi(uint64_t tmsi)
{
    char *content = NULL, *supi = NULL;
    cJSON *amf_info = NULL, *items = NULL, *ue = NULL;

    if (!tmsi)
        return NULL;

    content = xcn_http_get(XCN_AMF_UE_INFO_URL);
    if (!content)
        return NULL;

    amf_info = cJSON_Parse(content);
    ogs_free(content);
    if (!amf_info) {
        ogs_warn("Failed to parse AMF ue-info JSON");
        return NULL;
    }

    items = cJSON_GetObjectItemCaseSensitive(amf_info, "items");
    if (!cJSON_IsArray(items)) {
        cJSON_Delete(amf_info);
        return NULL;
    }

    cJSON_ArrayForEach(ue, items) {
        if (xcn_json_uint64(ue, "m_tmsi") == tmsi) {
            const char *ue_supi = xcn_json_string(ue, "supi");
            if (ue_supi)
                supi = ogs_strdup(ue_supi);
            break;
        }
    }

    cJSON_Delete(amf_info);
    return supi;
}

static bool xcn_amf_ue_info_match_session(
        cJSON *ue, const char *supi, uint8_t psi,
        uint64_t *amf_ue_ngap_id, uint64_t *ran_ue_ngap_id)
{
    const char *ue_supi = NULL;
    cJSON *sessions = NULL, *session = NULL, *gnb = NULL;

    ogs_assert(ue);
    ogs_assert(supi);
    ogs_assert(amf_ue_ngap_id);
    ogs_assert(ran_ue_ngap_id);

    ue_supi = xcn_json_string(ue, "supi");
    if (!ue_supi || strcmp(ue_supi, supi) != 0)
        return false;

    sessions = cJSON_GetObjectItemCaseSensitive(ue, "pdu_sessions");
    if (!cJSON_IsArray(sessions))
        return false;

    cJSON_ArrayForEach(session, sessions) {
        if (xcn_json_int(session, "psi", 0) == psi) {
            gnb = cJSON_GetObjectItemCaseSensitive(ue, "gnb");
            if (!cJSON_IsObject(gnb))
                return false;

            *amf_ue_ngap_id = xcn_json_uint64(gnb, "amf_ue_ngap_id");
            *ran_ue_ngap_id = xcn_json_uint64(gnb, "ran_ue_ngap_id");
            return *amf_ue_ngap_id || *ran_ue_ngap_id;
        }
    }

    return false;
}

static bool xcn_sess_store_ngap_ids_from_amf_info(
        pcf_ue_sm_t *pcf_ue_sm, pcf_sess_t *sess, cJSON *amf_info)
{
    cJSON *items = NULL, *ue = NULL;
    uint64_t amf_ue_ngap_id = 0;
    uint64_t ran_ue_ngap_id = 0;

    ogs_assert(pcf_ue_sm);
    ogs_assert(sess);
    ogs_assert(amf_info);

    items = cJSON_GetObjectItemCaseSensitive(amf_info, "items");
    if (!cJSON_IsArray(items))
        return false;

    cJSON_ArrayForEach(ue, items) {
        if (xcn_amf_ue_info_match_session(ue, pcf_ue_sm->supi, sess->psi,
                    &amf_ue_ngap_id, &ran_ue_ngap_id)) {
            pcf_sess_set_ngap_ids(sess, amf_ue_ngap_id, ran_ue_ngap_id);
            return true;
        }
    }

    return false;
}

void pcf_xcn_refresh_ngap_ids_from_amf(void)
{
    char *content = NULL;
    cJSON *amf_info = NULL;
    pcf_context_t *self = pcf_self();
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    pcf_sess_t *sess = NULL;

    content = xcn_http_get(XCN_AMF_UE_INFO_URL);
    if (!content)
        return;

    amf_info = cJSON_Parse(content);
    ogs_free(content);
    if (!amf_info) {
        ogs_warn("Failed to parse AMF ue-info JSON");
        return;
    }

    ogs_list_for_each(&self->pcf_ue_sm_list, pcf_ue_sm) {
        ogs_list_for_each(&pcf_ue_sm->sess_list, sess)
            xcn_sess_store_ngap_ids_from_amf_info(
                    pcf_ue_sm, sess, amf_info);
    }

    cJSON_Delete(amf_info);
}

static int xcn_json_int_from_any(
        cJSON *item, const char *key1, const char *key2, int default_value)
{
    int value = xcn_json_int(item, key1, default_value);

    if (value != default_value || !key2)
        return value;

    return xcn_json_int(item, key2, default_value);
}

static const char *xcn_json_string_from_any(
        cJSON *item, const char *key1, const char *key2)
{
    const char *value = xcn_json_string(item, key1);

    if (value || !key2)
        return value;

    return xcn_json_string(item, key2);
}

static uint8_t xcn_preemption_from_json(
        const char *value, const char **error_detail)
{
    ogs_assert(error_detail);

    if (!value)
        return 0;

    if (!ogs_strcasecmp(value, "enabled") ||
        !ogs_strcasecmp(value, "may_preempt") ||
        !ogs_strcasecmp(value, "may-preempt") ||
        !ogs_strcasecmp(value, "preempt"))
        return OGS_5GC_PRE_EMPTION_ENABLED;

    if (!ogs_strcasecmp(value, "disabled") ||
        !ogs_strcasecmp(value, "not_preempt") ||
        !ogs_strcasecmp(value, "not-preempt") ||
        !ogs_strcasecmp(value, "not preempt"))
        return OGS_5GC_PRE_EMPTION_DISABLED;

    *error_detail = "Invalid qos.arp.preemption value";
    return 0;
}

static uint8_t xcn_preemption_vulnerability_from_json(
        const char *value, const char **error_detail)
{
    ogs_assert(error_detail);

    if (!value)
        return 0;

    if (!ogs_strcasecmp(value, "enabled") ||
        !ogs_strcasecmp(value, "preemptable") ||
        !ogs_strcasecmp(value, "pre-emptable"))
        return OGS_5GC_PRE_EMPTION_ENABLED;

    if (!ogs_strcasecmp(value, "disabled") ||
        !ogs_strcasecmp(value, "not_preemptable") ||
        !ogs_strcasecmp(value, "not-preemptable") ||
        !ogs_strcasecmp(value, "not preemptable"))
        return OGS_5GC_PRE_EMPTION_DISABLED;

    *error_detail = "Invalid qos.arp.preemptionVulnerability";
    return 0;
}

static bool xcn_parse_qos_override(
        cJSON *item, ogs_pcc_rule_t *pcc_rule, const char **error_detail)
{
    cJSON *qos = NULL, *arp = NULL;
    int qos_index = 0, priority_level = 8, precedence = 100;
    const char *value = NULL;

    ogs_assert(item);
    ogs_assert(pcc_rule);
    ogs_assert(error_detail);

    qos = cJSON_GetObjectItemCaseSensitive(item, "qos");
    if (!qos)
        return true;

    if (!cJSON_IsObject(qos)) {
        *error_detail = "qos must be an object";
        return false;
    }

    qos_index = xcn_json_int_from_any(qos, "5qi", "index", 0);
    if (qos_index <= 0 || qos_index > UINT8_MAX) {
        *error_detail = "qos.5qi or qos.index must be 1..255";
        return false;
    }

    arp = cJSON_GetObjectItemCaseSensitive(qos, "arp");
    if (arp && !cJSON_IsObject(arp)) {
        *error_detail = "qos.arp must be an object";
        return false;
    }

    if (arp)
        priority_level = xcn_json_int(arp, "priorityLevel", priority_level);
    priority_level = xcn_json_int(qos, "arpPriorityLevel", priority_level);
    if (priority_level < 1 || priority_level > 15) {
        *error_detail = "qos.arp.priorityLevel must be 1..15";
        return false;
    }

    precedence = xcn_json_int(qos, "precedence", precedence);
    if (precedence < 0) {
        *error_detail = "qos.precedence must be >= 0";
        return false;
    }

    memset(pcc_rule, 0, sizeof(*pcc_rule));
    pcc_rule->id = (char *)XCN_SVC_DEDICATED_BEARER;
    pcc_rule->qos.index = (uint8_t)qos_index;
    pcc_rule->qos.arp.priority_level = (uint8_t)priority_level;

    value = arp ? xcn_json_string_from_any(arp,
            "preemptionCapability", "preemptCap") : NULL;
    if (!value)
        value = xcn_json_string_from_any(qos,
                "preemptionCapability", "preemptCap");
    pcc_rule->qos.arp.pre_emption_capability =
        xcn_preemption_from_json(value, error_detail);
    if (*error_detail)
        return false;
    if (!pcc_rule->qos.arp.pre_emption_capability)
        pcc_rule->qos.arp.pre_emption_capability =
            OGS_5GC_PRE_EMPTION_DISABLED;

    value = arp ? xcn_json_string_from_any(arp,
            "preemptionVulnerability", "preemptVuln") : NULL;
    if (!value)
        value = xcn_json_string_from_any(qos,
                "preemptionVulnerability", "preemptVuln");
    pcc_rule->qos.arp.pre_emption_vulnerability =
        xcn_preemption_vulnerability_from_json(value, error_detail);
    if (*error_detail)
        return false;
    if (!pcc_rule->qos.arp.pre_emption_vulnerability)
        pcc_rule->qos.arp.pre_emption_vulnerability =
            OGS_5GC_PRE_EMPTION_ENABLED;

    value = xcn_json_string_from_any(qos, "maxbrDl", "mbrDl");
    if (value)
        pcc_rule->qos.mbr.downlink = ogs_sbi_bitrate_from_string((char *)value);
    value = xcn_json_string_from_any(qos, "maxbrUl", "mbrUl");
    if (value)
        pcc_rule->qos.mbr.uplink = ogs_sbi_bitrate_from_string((char *)value);
    value = xcn_json_string(qos, "gbrDl");
    if (value)
        pcc_rule->qos.gbr.downlink = ogs_sbi_bitrate_from_string((char *)value);
    value = xcn_json_string(qos, "gbrUl");
    if (value)
        pcc_rule->qos.gbr.uplink = ogs_sbi_bitrate_from_string((char *)value);

    pcc_rule->flow_status = OpenAPI_flow_status_ENABLED;
    pcc_rule->precedence = (uint32_t)precedence;

    return true;
}

static const char *xcn_supi_to_imsi(const char *supi)
{
    if (supi && !ogs_strncasecmp(supi, "imsi-", strlen("imsi-")))
        return supi + strlen("imsi-");

    return supi;
}

static OpenAPI_media_type_e xcn_media_type_from_json(cJSON *item)
{
    const char *media_type = xcn_json_string(item, "mediaType");

    if (!media_type)
        return OpenAPI_media_type_NULL;

    if (!ogs_strcasecmp(media_type, "audio"))
        return OpenAPI_media_type_AUDIO;
    if (!ogs_strcasecmp(media_type, "video"))
        return OpenAPI_media_type_VIDEO;
    if (!ogs_strcasecmp(media_type, "control"))
        return OpenAPI_media_type_CONTROL;

    return OpenAPI_media_type_FromString((char *)media_type);
}

static cJSON *xcn_session_to_json(pcf_sess_t *sess)
{
    cJSON *item = NULL, *snssai = NULL, *routes = NULL;
    OpenAPI_lnode_t *node = NULL;

    ogs_assert(sess);

    item = cJSON_CreateObject();
    ogs_assert(item);

    cJSON_AddNumberToObject(item, "pduSessionId", sess->psi);
    if (sess->dnn)
        cJSON_AddStringToObject(item, "dnn", sess->dnn);
    if (sess->full_dnn)
        cJSON_AddStringToObject(item, "fullDnn", sess->full_dnn);
    cJSON_AddNumberToObject(item, "pduSessionType", sess->pdu_session_type);
    if (sess->ipv4addr_string)
        cJSON_AddStringToObject(item, "ipv4", sess->ipv4addr_string);
    if (sess->ipv6prefix_string)
        cJSON_AddStringToObject(item, "ipv6Prefix", sess->ipv6prefix_string);
    if (sess->amf_ue_ngap_id)
        cJSON_AddNumberToObject(item, "amfUeNgapId", sess->amf_ue_ngap_id);
    if (sess->ran_ue_ngap_id)
        cJSON_AddNumberToObject(item, "ranUeNgapId", sess->ran_ue_ngap_id);

    snssai = cJSON_AddObjectToObject(item, "sNssai");
    ogs_assert(snssai);
    cJSON_AddNumberToObject(snssai, "sst", sess->s_nssai.sst);
    if (sess->s_nssai.sd.v != OGS_S_NSSAI_NO_SD_VALUE) {
        char *sd = ogs_s_nssai_sd_to_string(sess->s_nssai.sd);
        if (sd) {
            cJSON_AddStringToObject(snssai, "sd", sd);
            ogs_free(sd);
        }
    }

    if (sess->ipv4_frame_route_list) {
        routes = cJSON_AddArrayToObject(item, "ipv4FrameRoutes");
        ogs_assert(routes);
        OpenAPI_list_for_each(sess->ipv4_frame_route_list, node) {
            if (node->data)
                cJSON_AddItemToArray(routes,
                        cJSON_CreateString((char *)node->data));
        }
    }

    if (sess->ipv6_frame_route_list) {
        routes = cJSON_AddArrayToObject(item, "ipv6FrameRoutes");
        ogs_assert(routes);
        OpenAPI_list_for_each(sess->ipv6_frame_route_list, node) {
            if (node->data)
                cJSON_AddItemToArray(routes,
                        cJSON_CreateString((char *)node->data));
        }
    }

    return item;
}

static cJSON *xcn_user_to_json(pcf_ue_sm_t *pcf_ue_sm)
{
    cJSON *item = NULL, *sessions = NULL;
    pcf_sess_t *sess = NULL;

    ogs_assert(pcf_ue_sm);

    item = cJSON_CreateObject();
    ogs_assert(item);

    cJSON_AddStringToObject(item, "supi", pcf_ue_sm->supi);
    cJSON_AddStringToObject(item, "imsi", xcn_supi_to_imsi(pcf_ue_sm->supi));
    cJSON_AddBoolToObject(item, "registered",
            pcf_ue_am_find_by_supi(pcf_ue_sm->supi) ? true : false);

    sessions = cJSON_AddArrayToObject(item, "sessions");
    ogs_assert(sessions);
    ogs_list_for_each(&pcf_ue_sm->sess_list, sess)
        cJSON_AddItemToArray(sessions, xcn_session_to_json(sess));

    return item;
}

static bool xcn_parse_bearer_request(cJSON *item, pcf_sess_t **sess)
{
    const char *supi = NULL;
    const char *ue_ip = NULL;
    uint64_t ngap_id = 0;
    uint64_t amf_ue_ngap_id = 0;
    uint64_t ran_ue_ngap_id = 0;
    int pdu_session_id = 0;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    ogs_assert(item);
    ogs_assert(sess);

    *sess = NULL;

    ue_ip = xcn_json_string_from_any(item, "ueIp", "ueIpAddr");
    if (!ue_ip)
        ue_ip = xcn_json_string_from_any(item, "ueIpv4", "ueIpv6");
    if (ue_ip) {
        *sess = pcf_sess_find_by_ipv4addr((char *)ue_ip);
        if (!*sess)
            *sess = pcf_sess_find_by_ipv6addr((char *)ue_ip);
        if (!*sess)
            *sess = pcf_sess_find_by_ipv6prefix((char *)ue_ip);
        return *sess ? true : false;
    }

    amf_ue_ngap_id = xcn_json_uint64(item, "amfUeNgapId");
    ran_ue_ngap_id = xcn_json_uint64(item, "ranUeNgapId");
    ngap_id = xcn_json_uint64(item, "ngapId");
    if (amf_ue_ngap_id || ran_ue_ngap_id || ngap_id) {
        pcf_xcn_refresh_ngap_ids_from_amf();
        if (amf_ue_ngap_id)
            *sess = pcf_sess_find_by_amf_ue_ngap_id(amf_ue_ngap_id);
        if (!*sess && ngap_id)
            *sess = pcf_sess_find_by_amf_ue_ngap_id(ngap_id);
        if (!*sess && ran_ue_ngap_id)
            *sess = pcf_sess_find_by_ran_ue_ngap_id(ran_ue_ngap_id);
        if (!*sess && ngap_id)
            *sess = pcf_sess_find_by_ran_ue_ngap_id(ngap_id);
        return *sess ? true : false;
    }

    supi = xcn_json_string(item, "supi");
    pdu_session_id = xcn_json_int(item, "pduSessionId", 0);
    if (!supi || pdu_session_id <= 0 || pdu_session_id > UINT8_MAX)
        return false;

    pcf_ue_sm = pcf_ue_sm_find_by_supi((char *)supi);
    if (!pcf_ue_sm)
        return false;

    *sess = pcf_sess_find_by_psi(pcf_ue_sm, (uint8_t)pdu_session_id);
    return *sess ? true : false;
}

static bool xcn_parse_bearer_params(ogs_hash_t *params, pcf_sess_t **sess)
{
    const char *supi = NULL;
    const char *ue_ip = NULL;
    const char *pdu_session_id_string = NULL;
    uint64_t ngap_id = 0;
    uint64_t amf_ue_ngap_id = 0;
    uint64_t ran_ue_ngap_id = 0;
    uint64_t pdu_session_id = 0;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    ogs_assert(sess);

    *sess = NULL;

    if (!params)
        return false;

    ue_ip = ogs_sbi_header_get(params, "ueIp");
    if (!ue_ip)
        ue_ip = ogs_sbi_header_get(params, "ueIpAddr");
    if (!ue_ip)
        ue_ip = ogs_sbi_header_get(params, "ueIpv4");
    if (!ue_ip)
        ue_ip = ogs_sbi_header_get(params, "ueIpv6");
    if (ue_ip) {
        *sess = pcf_sess_find_by_ipv4addr((char *)ue_ip);
        if (!*sess)
            *sess = pcf_sess_find_by_ipv6addr((char *)ue_ip);
        if (!*sess)
            *sess = pcf_sess_find_by_ipv6prefix((char *)ue_ip);
        return *sess ? true : false;
    }

    amf_ue_ngap_id =
        xcn_string_uint64(ogs_sbi_header_get(params, "amfUeNgapId"));
    ran_ue_ngap_id =
        xcn_string_uint64(ogs_sbi_header_get(params, "ranUeNgapId"));
    ngap_id = xcn_string_uint64(ogs_sbi_header_get(params, "ngapId"));
    if (amf_ue_ngap_id || ran_ue_ngap_id || ngap_id) {
        pcf_xcn_refresh_ngap_ids_from_amf();
        if (amf_ue_ngap_id)
            *sess = pcf_sess_find_by_amf_ue_ngap_id(amf_ue_ngap_id);
        if (!*sess && ngap_id)
            *sess = pcf_sess_find_by_amf_ue_ngap_id(ngap_id);
        if (!*sess && ran_ue_ngap_id)
            *sess = pcf_sess_find_by_ran_ue_ngap_id(ran_ue_ngap_id);
        if (!*sess && ngap_id)
            *sess = pcf_sess_find_by_ran_ue_ngap_id(ngap_id);
        return *sess ? true : false;
    }

    supi = ogs_sbi_header_get(params, "supi");
    pdu_session_id_string = ogs_sbi_header_get(params, "pduSessionId");
    pdu_session_id = xcn_string_uint64(pdu_session_id_string);
    if (!supi || pdu_session_id == 0 || pdu_session_id > UINT8_MAX)
        return false;

    pcf_ue_sm = pcf_ue_sm_find_by_supi((char *)supi);
    if (!pcf_ue_sm)
        return false;

    *sess = pcf_sess_find_by_psi(pcf_ue_sm, (uint8_t)pdu_session_id);
    return *sess ? true : false;
}

void pcf_xcn_store_ngap_ids_from_sm_policy_content(
        pcf_sess_t *sess, const char *content)
{
    cJSON *item = NULL;
    uint64_t amf_ue_ngap_id = 0;
    uint64_t ran_ue_ngap_id = 0;

    ogs_assert(sess);

    if (!content)
        return;

    item = cJSON_Parse(content);
    if (!item)
        return;

    amf_ue_ngap_id = xcn_json_uint64(item, "xcnAmfUeNgapId");
    ran_ue_ngap_id = xcn_json_uint64(item, "xcnRanUeNgapId");
    if (amf_ue_ngap_id || ran_ue_ngap_id)
        pcf_sess_set_ngap_ids(sess, amf_ue_ngap_id, ran_ue_ngap_id);

    cJSON_Delete(item);
}

bool pcf_npcf_am_policy_control_handle_create(pcf_ue_am_t *pcf_ue_am,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *message)
{
    bool rc;
    int r;

    OpenAPI_policy_association_request_t *PolicyAssociationRequest = NULL;
    OpenAPI_guami_t *Guami = NULL;
    OpenAPI_lnode_t *node = NULL;

    uint64_t supported_features = 0;

    ogs_sbi_server_t *server = NULL;
    ogs_sbi_client_t *client = NULL;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

    ogs_assert(pcf_ue_am);
    ogs_assert(stream);
    server = ogs_sbi_server_from_stream(stream);
    ogs_assert(server);
    ogs_assert(message);

    PolicyAssociationRequest = message->PolicyAssociationRequest;
    if (!PolicyAssociationRequest) {
        ogs_error("[%s] No PolicyAssociationRequest", pcf_ue_am->supi);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                message, "[%s] No PolicyAssociationRequest", pcf_ue_am->supi,
                NULL));
        return false;
    }

    if (!PolicyAssociationRequest->notification_uri) {
        ogs_error("[%s] No notificationUri", pcf_ue_am->supi);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                message, "No notificationUri", pcf_ue_am->supi, NULL));
        return false;
    }

    if (!PolicyAssociationRequest->supi) {
        ogs_error("[%s] No supi", pcf_ue_am->supi);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                message, "No supi", pcf_ue_am->supi, NULL));
        return false;
    }

    if (!PolicyAssociationRequest->supp_feat) {
        ogs_error("[%s] No suppFeat", pcf_ue_am->supi);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                message, "No suppFeat", pcf_ue_am->supi, NULL));
        return false;
    }

    rc = ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &fqdn_port, &addr, &addr6,
            PolicyAssociationRequest->notification_uri);
    if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
        ogs_error("[%s] Invalid URI [%s]",
                pcf_ue_am->supi, PolicyAssociationRequest->notification_uri);
        ogs_assert(true ==
            ogs_sbi_server_send_error(stream, OGS_SBI_HTTP_STATUS_BAD_REQUEST,
                message, "[%s] Invalid URI", pcf_ue_am->supi, NULL));
        return false;
    }

    if (pcf_ue_am->notification_uri)
        ogs_free(pcf_ue_am->notification_uri);
    pcf_ue_am->notification_uri = ogs_strdup(
            PolicyAssociationRequest->notification_uri);
    ogs_assert(pcf_ue_am->notification_uri);

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client) {
        ogs_debug("%s: ogs_sbi_client_add()", OGS_FUNC);
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);
        if (!client) {
            ogs_error("%s: ogs_sbi_client_add() failed", OGS_FUNC);

            ogs_free(fqdn);
            ogs_freeaddrinfo(addr);
            ogs_freeaddrinfo(addr6);

            return false;
        }
    }
    OGS_SBI_SETUP_CLIENT(&pcf_ue_am->namf, client);

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);

    supported_features =
        ogs_uint64_from_string_hexadecimal(
                PolicyAssociationRequest->supp_feat);
    pcf_ue_am->am_policy_control_features &= supported_features;

    if (PolicyAssociationRequest->gpsi) {
        if (pcf_ue_am->gpsi)
            ogs_free(pcf_ue_am->gpsi);
        pcf_ue_am->gpsi = ogs_strdup(PolicyAssociationRequest->gpsi);
    }

    pcf_ue_am->access_type = PolicyAssociationRequest->access_type;

    if (PolicyAssociationRequest->pei) {
        if (pcf_ue_am->pei)
            ogs_free(pcf_ue_am->pei);
        pcf_ue_am->pei = ogs_strdup(PolicyAssociationRequest->pei);
    }

    Guami = PolicyAssociationRequest->guami;
    if (Guami && Guami->amf_id &&
        Guami->plmn_id && Guami->plmn_id->mnc && Guami->plmn_id->mcc) {
        ogs_sbi_parse_guami(&pcf_ue_am->guami, PolicyAssociationRequest->guami);
    }

    OpenAPI_list_for_each(PolicyAssociationRequest->allowed_snssais, node) {
        struct OpenAPI_snssai_s *Snssai = node->data;
        if (Snssai) {
            ogs_s_nssai_t s_nssai;
            s_nssai.sst = Snssai->sst;
            s_nssai.sd = ogs_s_nssai_sd_from_string(Snssai->sd);

            pcf_metrics_inst_by_slice_add(&pcf_ue_am->guami.plmn_id,
                    &s_nssai, PCF_METR_CTR_PA_POLICYAMASSOREQ, 1);
        } else {
            ogs_error("[%s] No Snssai", pcf_ue_am->supi);
        }
    }

    if (PolicyAssociationRequest->rat_type)
        pcf_ue_am->rat_type = PolicyAssociationRequest->rat_type;

    pcf_ue_am->policy_association_request =
        OpenAPI_policy_association_request_copy(
                pcf_ue_am->policy_association_request,
                message->PolicyAssociationRequest);

    if (PolicyAssociationRequest->ue_ambr)
        pcf_ue_am->subscribed_ue_ambr = OpenAPI_ambr_copy(
                pcf_ue_am->subscribed_ue_ambr,
                PolicyAssociationRequest->ue_ambr);

    if (ogs_sbi_supi_in_vplmn(pcf_ue_am->supi) == true) {
        /* Visited PLMN */
        OpenAPI_policy_association_t PolicyAssociation;

        ogs_sbi_message_t sendmsg;
        ogs_sbi_header_t header;
        ogs_sbi_response_t *response = NULL;

        memset(&PolicyAssociation, 0, sizeof(PolicyAssociation));
        PolicyAssociation.request = pcf_ue_am->policy_association_request;
        PolicyAssociation.supp_feat =
            ogs_uint64_to_string(pcf_ue_am->am_policy_control_features);
        ogs_assert(PolicyAssociation.supp_feat);

        memset(&header, 0, sizeof(header));
        header.service.name =
            (char *)OGS_SBI_SERVICE_NAME_NPCF_AM_POLICY_CONTROL;
        header.api.version = (char *)OGS_SBI_API_V1;
        header.resource.component[0] = (char *)OGS_SBI_RESOURCE_NAME_POLICIES;
        header.resource.component[1] = pcf_ue_am->association_id;

        memset(&sendmsg, 0, sizeof(sendmsg));
        sendmsg.PolicyAssociation = &PolicyAssociation;
        sendmsg.http.location = ogs_sbi_server_uri(server, &header);

        response = ogs_sbi_build_response(
                &sendmsg, OGS_SBI_HTTP_STATUS_CREATED);
        ogs_assert(response);
        ogs_assert(true == ogs_sbi_server_send_response(stream, response));

        ogs_free(sendmsg.http.location);

        ogs_free(PolicyAssociation.supp_feat);

        return true;
    } else {
        /* Home PLMN */
        r = pcf_ue_am_sbi_discover_and_send(OGS_SBI_SERVICE_TYPE_NUDR_DR, NULL,
                pcf_nudr_dr_build_query_am_data, pcf_ue_am, stream, NULL);
        ogs_expect(r == OGS_OK);
        ogs_assert(r != OGS_ERROR);

        return (r == OGS_OK);
    }
}

bool pcf_npcf_smpolicycontrol_handle_create(pcf_sess_t *sess,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *message)
{
    bool rc;
    int status = 0;
    int r;
    char *strerror = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    OpenAPI_sm_policy_context_data_t *SmPolicyContextData = NULL;
    OpenAPI_plmn_id_nid_t *servingNetwork = NULL;
    OpenAPI_snssai_t *sliceInfo = NULL;

    ogs_sbi_client_t *client = NULL;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

    char *dnn_oi = NULL;

    ogs_assert(sess);
    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(stream);
    ogs_assert(message);

    SmPolicyContextData = message->SmPolicyContextData;
    if (!SmPolicyContextData) {
        strerror = ogs_msprintf("[%s:%d] No SmPolicyContextData",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->supi) {
        strerror = ogs_msprintf("[%s:%d] No supi", pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->pdu_session_id) {
        strerror = ogs_msprintf("[%s:%d] No pduSessionId",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->pdu_session_type) {
        strerror = ogs_msprintf("[%s:%d] No pduSessionType",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->dnn) {
        strerror = ogs_msprintf("[%s:%d] No dnn", pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->notification_uri) {
        strerror = ogs_msprintf("[%s:%d] No notificationUri",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!SmPolicyContextData->ipv4_address &&
        !SmPolicyContextData->ipv6_address_prefix) {
        strerror = ogs_msprintf(
                "[%s:%d] No IPv4 address[%p] or IPv6 prefix[%p]",
                pcf_ue_sm->supi, sess->psi,
                SmPolicyContextData->ipv4_address,
                SmPolicyContextData->ipv6_address_prefix);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    sliceInfo = SmPolicyContextData->slice_info;
    if (!sliceInfo) {
        strerror = ogs_msprintf("[%s:%d] No sliceInfo",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    servingNetwork = SmPolicyContextData->serving_network;
    if (servingNetwork) {
        if (!servingNetwork->mcc) {
            strerror = ogs_msprintf("[%s:%d] No servingNetwork->mcc",
                    pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }
        if (!servingNetwork->mnc) {
            strerror = ogs_msprintf("[%s:%d] No servingNetwork->mnc",
                    pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }
    } else {
        ogs_warn("No servingNetwork");
    }

    rc = ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &fqdn_port, &addr, &addr6,
            SmPolicyContextData->notification_uri);
    if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
        strerror = ogs_msprintf("[%s:%d] Invalid URI [%s]",
                pcf_ue_sm->supi, sess->psi,
                SmPolicyContextData->notification_uri);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (SmPolicyContextData->gpsi) {
        if (pcf_ue_sm->gpsi)
            ogs_free(pcf_ue_sm->gpsi);
        pcf_ue_sm->gpsi = ogs_strdup(SmPolicyContextData->gpsi);
    }

    if (SmPolicyContextData->supp_feat) {
        uint64_t supported_features =
            ogs_uint64_from_string_hexadecimal(
                    SmPolicyContextData->supp_feat);
        sess->smpolicycontrol_features &= supported_features;
    } else {
        sess->smpolicycontrol_features = 0;
    }

    sess->pdu_session_type = SmPolicyContextData->pdu_session_type;

    /* Serving PLMN & Home PLMN */
    if (servingNetwork) {
        sess->serving.presence = true;
        ogs_sbi_parse_plmn_id_nid(&sess->serving.plmn_id, servingNetwork);

        sess->home.presence = true;
        memcpy(&sess->home.plmn_id, &sess->serving.plmn_id, OGS_PLMN_ID_LEN);
    }

    /*
     * TS29.512
     * 5 Npcf_SMPolicyControl Service API
     * 5.6 Data Model
     * 5.6.2 Structured data types
     * Table 5.6.2.3-1: Definition of type SmPolicyContextData
     *
     * NAME: dnn
     * Data type: Dnn
     * P: M
     * Cardinality: 1
     * The DNN of the PDU session, a full DNN with both the Network Identifier
     * and Operator Identifier, or a DNN with the Network Identifier only
     */
    dnn_oi = ogs_dnn_oi_from_fqdn(SmPolicyContextData->dnn);

    if (dnn_oi) {
        char dnn_ni[OGS_MAX_DNN_LEN+1];
        uint16_t mcc = 0, mnc = 0;

        ogs_assert(dnn_oi > SmPolicyContextData->dnn);

        ogs_cpystrn(dnn_ni, SmPolicyContextData->dnn,
            ogs_min(OGS_MAX_DNN_LEN, dnn_oi - SmPolicyContextData->dnn));

        if (sess->dnn)
            ogs_free(sess->dnn);
        sess->dnn = ogs_strdup(dnn_ni);
        ogs_assert(sess->dnn);

        if (sess->full_dnn)
            ogs_free(sess->full_dnn);
        sess->full_dnn = ogs_strdup(SmPolicyContextData->dnn);
        ogs_assert(sess->full_dnn);

        mcc = ogs_plmn_id_mcc_from_fqdn(sess->full_dnn);
        mnc = ogs_plmn_id_mnc_from_fqdn(sess->full_dnn);

        /*
         * To generate the Home PLMN ID of the SMF-UE,
         * the length of the MNC is obtained
         * by comparing the MNC part of the SUPI and full-DNN.
         */
        if (mcc && mnc &&
            strncmp(pcf_ue_sm->supi, "imsi-", strlen("imsi-")) == 0) {
            int mnc_len = 0;
            char buf[OGS_PLMNIDSTRLEN];

            ogs_snprintf(buf, OGS_PLMNIDSTRLEN, "%03d%02d", mcc, mnc);
            if (strncmp(pcf_ue_sm->supi + 5, buf, strlen(buf)) == 0)
                mnc_len = 2;

            ogs_snprintf(buf, OGS_PLMNIDSTRLEN, "%03d%03d", mcc, mnc);
            if (strncmp(pcf_ue_sm->supi + 5, buf, strlen(buf)) == 0)
                mnc_len = 3;

            /* Change Home PLMN for VPLMN */
            if (mnc_len == 2 || mnc_len == 3) {
                if (sess->home.presence == true)
                    ogs_plmn_id_build(&sess->home.plmn_id, mcc, mnc, mnc_len);
            }
        }
    } else {
        if (sess->dnn)
            ogs_free(sess->dnn);
        sess->dnn = ogs_strdup(SmPolicyContextData->dnn);
        ogs_assert(sess->dnn);

        if (sess->full_dnn)
            ogs_free(sess->full_dnn);
        sess->full_dnn = NULL;
    }

    if (sess->notification_uri)
        ogs_free(sess->notification_uri);
    sess->notification_uri = ogs_strdup(SmPolicyContextData->notification_uri);
    ogs_assert(sess->notification_uri);

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client) {
        ogs_debug("%s: ogs_sbi_client_add()", OGS_FUNC);
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);
        if (!client) {
            strerror = ogs_msprintf("%s: ogs_sbi_client_add() failed",
                    OGS_FUNC);
            status = OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ogs_freeaddrinfo(addr);
            goto cleanup;
        }
    }
    OGS_SBI_SETUP_CLIENT(&sess->nsmf, client);

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);

    if (SmPolicyContextData->ipv4_address)
        ogs_assert(true ==
            pcf_sess_set_ipv4addr(sess, SmPolicyContextData->ipv4_address));
    if (SmPolicyContextData->ipv6_address_prefix)
        ogs_assert(true ==
            pcf_sess_set_ipv6prefix(
                sess, SmPolicyContextData->ipv6_address_prefix));
    if (SmPolicyContextData->xcn_amf_ue_ngap_id ||
        SmPolicyContextData->xcn_ran_ue_ngap_id)
        pcf_sess_set_ngap_ids(sess,
                SmPolicyContextData->xcn_amf_ue_ngap_id,
                SmPolicyContextData->xcn_ran_ue_ngap_id);
    if (!sess->amf_ue_ngap_id && !sess->ran_ue_ngap_id)
        pcf_xcn_refresh_ngap_ids_from_amf();

    if (SmPolicyContextData->ipv4_frame_route_list) {
        OpenAPI_lnode_t *node = NULL;

        OpenAPI_clear_and_free_string_list(sess->ipv4_frame_route_list);
        sess->ipv4_frame_route_list = OpenAPI_list_create();
        OpenAPI_list_for_each(
                SmPolicyContextData->ipv4_frame_route_list, node) {
            if (!node->data)
                continue;
            OpenAPI_list_add(
                    sess->ipv4_frame_route_list, ogs_strdup(node->data));
        }
    }

    if (SmPolicyContextData->ipv6_frame_route_list) {
        OpenAPI_lnode_t *node = NULL;

        OpenAPI_clear_and_free_string_list(sess->ipv6_frame_route_list);
        sess->ipv6_frame_route_list = OpenAPI_list_create();
        OpenAPI_list_for_each(
                SmPolicyContextData->ipv6_frame_route_list, node) {
            if (!node->data)
                continue;
            OpenAPI_list_add(
                    sess->ipv6_frame_route_list, ogs_strdup(node->data));
        }
    }

    sess->s_nssai.sst = sliceInfo->sst;
    sess->s_nssai.sd = ogs_s_nssai_sd_from_string(sliceInfo->sd);

    pcf_metrics_inst_by_slice_add(
            sess->home.presence == true ? &sess->home.plmn_id : NULL,
            &sess->s_nssai, PCF_METR_GAUGE_PA_SESSIONNBR, 1);
    pcf_metrics_inst_by_slice_add(
            sess->home.presence == true ? &sess->home.plmn_id : NULL,
            &sess->s_nssai, PCF_METR_CTR_PA_POLICYSMASSOREQ, 1);

    if (SmPolicyContextData->subs_sess_ambr)
        sess->subscribed_sess_ambr = OpenAPI_ambr_copy(
            sess->subscribed_sess_ambr, SmPolicyContextData->subs_sess_ambr);

    if (SmPolicyContextData->subs_def_qos)
        sess->subscribed_default_qos = OpenAPI_subscribed_default_qos_copy(
            sess->subscribed_default_qos, SmPolicyContextData->subs_def_qos);

    if (ogs_sbi_supi_in_vplmn(pcf_ue_sm->supi) == true) {
        /* Visited PLMN */
        r = pcf_sess_sbi_discover_and_send(
                    OGS_SBI_SERVICE_TYPE_NBSF_MANAGEMENT, NULL,
                    pcf_nbsf_management_build_register,
                    sess, stream, NULL);
        ogs_expect(r == OGS_OK);
        ogs_assert(r != OGS_ERROR);

        return (r == OGS_OK);
    } else {
        /* Home PLMN */
        r = pcf_sess_sbi_discover_and_send(
                OGS_SBI_SERVICE_TYPE_NUDR_DR, NULL,
                pcf_nudr_dr_build_query_sm_data, sess, stream, NULL);
        ogs_expect(r == OGS_OK);
        ogs_assert(r != OGS_ERROR);

        return (r == OGS_OK);
    }

cleanup:
    ogs_assert(status);
    ogs_assert(strerror);
    ogs_error("%s", strerror);
    /*
     * TS29.512
     * 4.2.2.2 SM Policy Association establishment 
     *
     * If the PCF is, due to incomplete, erroneous or missing
     * information (e.g. QoS, RAT type, subscriber information)
     * not able to provision a policy decision as response to
     * the request for PCC rules by the SMF, the PCF may reject
     * the request and include in an HTTP "400 Bad Request"
     * response message the "cause" attribute of the ProblemDetails
     * data structure set to "ERROR_INITIAL_PARAMETERS". 
     */
    ogs_assert(true ==
            ogs_sbi_server_send_error(stream, status, message,
                    strerror, NULL, "ERROR_INITIAL_PARAMETERS"));
    ogs_free(strerror);

    return false;
}

bool pcf_npcf_smpolicycontrol_handle_delete(pcf_sess_t *sess,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *message)
{
    int r;
    int status = 0;
    char *strerror = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    pcf_app_t *app_session = NULL;

    OpenAPI_sm_policy_delete_data_t *SmPolicyDeleteData = NULL;

    ogs_assert(sess);
    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(stream);
    ogs_assert(message);

    SmPolicyDeleteData = message->SmPolicyDeleteData;
    if (!SmPolicyDeleteData) {
        strerror = ogs_msprintf("[%s:%d] No SmPolicyDeleteData",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    ogs_list_for_each(&sess->app_list, app_session) {
        pcf_sbi_send_policyauthorization_terminate_notify(app_session);
    }

    if (pcf_sessions_number_by_snssai_and_dnn(
                pcf_ue_sm, &sess->s_nssai, sess->dnn) > 1) {
        ogs_expect(true ==
                ogs_sbi_send_response(stream, OGS_SBI_HTTP_STATUS_NO_CONTENT));
    } else if (sess->binding.resource_uri) {
        r = pcf_sess_sbi_discover_and_send(
                OGS_SBI_SERVICE_TYPE_NBSF_MANAGEMENT, NULL,
                pcf_nbsf_management_build_de_register, sess, stream, NULL);
        ogs_expect(r == OGS_OK);
        ogs_assert(r != OGS_ERROR);
    } else {
        ogs_expect(true ==
                ogs_sbi_send_response(stream, OGS_SBI_HTTP_STATUS_NO_CONTENT));
    }

    return true;

cleanup:
    ogs_assert(status);
    ogs_assert(strerror);
    ogs_error("%s", strerror);
    ogs_assert(true ==
        ogs_sbi_server_send_error(stream, status, message, strerror, NULL,
                NULL));
    ogs_free(strerror);

    return false;
}

static void pcf_free_policy_decision_lists(
        OpenAPI_list_t **PccRuleList, OpenAPI_list_t **QosDecisionList)
{
    OpenAPI_lnode_t *node = NULL;
    OpenAPI_map_t *PccRuleMap = NULL, *QosDecisionMap = NULL;
    OpenAPI_pcc_rule_t *PccRule = NULL;
    OpenAPI_qos_data_t *QosData = NULL;

    if (PccRuleList && *PccRuleList) {
        OpenAPI_list_for_each(*PccRuleList, node) {
            PccRuleMap = node->data;
            if (PccRuleMap) {
                PccRule = PccRuleMap->value;
                if (PccRule)
                    ogs_sbi_free_pcc_rule(PccRule);
                ogs_free(PccRuleMap);
            }
        }
        OpenAPI_list_free(*PccRuleList);
        *PccRuleList = NULL;
    }

    if (QosDecisionList && *QosDecisionList) {
        OpenAPI_list_for_each(*QosDecisionList, node) {
            QosDecisionMap = node->data;
            if (QosDecisionMap) {
                QosData = QosDecisionMap->value;
                if (QosData)
                    ogs_sbi_free_qos_data(QosData);
                ogs_free(QosDecisionMap);
            }
        }
        OpenAPI_list_free(*QosDecisionList);
        *QosDecisionList = NULL;
    }
}

static bool pcf_build_app_policy_decision(
        pcf_sess_t *sess, OpenAPI_sm_policy_decision_t *SmPolicyDecision,
        OpenAPI_list_t **PccRuleList, OpenAPI_list_t **QosDecisionList)
{
    int i;
    pcf_app_t *app_session = NULL;

    ogs_assert(sess);
    ogs_assert(SmPolicyDecision);
    ogs_assert(PccRuleList);
    ogs_assert(QosDecisionList);

    memset(SmPolicyDecision, 0, sizeof(*SmPolicyDecision));

    *PccRuleList = OpenAPI_list_create();
    ogs_assert(*PccRuleList);
    *QosDecisionList = OpenAPI_list_create();
    ogs_assert(*QosDecisionList);

    ogs_list_for_each(&sess->app_list, app_session) {
        for (i = 0; i < app_session->num_of_pcc_rule; i++) {
            OpenAPI_pcc_rule_t *PccRule = NULL;
            OpenAPI_qos_data_t *QosData = NULL;
            OpenAPI_map_t *PccRuleMap = NULL, *QosDecisionMap = NULL;
            ogs_pcc_rule_t *pcc_rule = &app_session->pcc_rule[i];

            ogs_assert(pcc_rule);

            if (!pcc_rule->id || !pcc_rule->num_of_flow)
                continue;

            PccRule = ogs_sbi_build_pcc_rule(pcc_rule, 1);
            ogs_assert(PccRule);
            ogs_assert(PccRule->pcc_rule_id);

            PccRuleMap = OpenAPI_map_create(PccRule->pcc_rule_id, PccRule);
            ogs_assert(PccRuleMap);
            OpenAPI_list_add(*PccRuleList, PccRuleMap);

            QosData = ogs_sbi_build_qos_data(pcc_rule);
            ogs_assert(QosData);
            ogs_assert(QosData->qos_id);

            QosDecisionMap = OpenAPI_map_create(QosData->qos_id, QosData);
            ogs_assert(QosDecisionMap);
            OpenAPI_list_add(*QosDecisionList, QosDecisionMap);
        }
    }

    if ((*PccRuleList)->count)
        SmPolicyDecision->pcc_rules = *PccRuleList;

    if ((*QosDecisionList)->count)
        SmPolicyDecision->qos_decs = *QosDecisionList;

    return (*PccRuleList)->count || (*QosDecisionList)->count;
}

bool pcf_npcf_policyauthorization_handle_create(pcf_sess_t *sess,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg,
        ogs_pcc_rule_t *xcn_qos_override)
{
    bool rc;
    int i, j, rv, status = 0;
    char *strerror = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    pcf_app_t *app_session = NULL;

    ogs_sbi_client_t *client = NULL;
    OpenAPI_uri_scheme_e scheme = OpenAPI_uri_scheme_NULL;
    char *fqdn = NULL;
    uint16_t fqdn_port = 0;
    ogs_sockaddr_t *addr = NULL, *addr6 = NULL;

    OpenAPI_app_session_context_t *AppSessionContext = NULL;
    OpenAPI_app_session_context_req_data_t *AscReqData = NULL;

    uint64_t supported_features = 0;

    ogs_sbi_server_t *server = NULL;
    ogs_sbi_header_t header;
    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;

    ogs_session_data_t session_data;

    ogs_ims_data_t ims_data;
    ogs_media_component_t *media_component = NULL;
    ogs_media_sub_component_t *sub = NULL;

    OpenAPI_list_t *MediaComponentList = NULL;
    OpenAPI_map_t *MediaComponentMap = NULL;
    OpenAPI_media_component_t *MediaComponent = NULL;

    OpenAPI_list_t *SubComponentList = NULL;
    OpenAPI_map_t *SubComponentMap = NULL;
    OpenAPI_media_sub_component_t *SubComponent = NULL;

    OpenAPI_list_t *fDescList = NULL;

    OpenAPI_sm_policy_decision_t SmPolicyDecision;

    OpenAPI_list_t *PccRuleList = NULL;
    OpenAPI_map_t *PccRuleMap = NULL;
    OpenAPI_pcc_rule_t *PccRule = NULL;

    OpenAPI_list_t *QosDecisionList = NULL;
    OpenAPI_map_t *QosDecisionMap = NULL;
    OpenAPI_qos_data_t *QosData = NULL;

    OpenAPI_lnode_t *node = NULL, *node2 = NULL, *node3 = NULL;

    ogs_assert(sess);
    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    server = ogs_sbi_server_from_stream(stream);
    ogs_assert(server);

    memset(&ims_data, 0, sizeof(ims_data));
    memset(&session_data, 0, sizeof(ogs_session_data_t));

    AppSessionContext = recvmsg->AppSessionContext;
    if (!AppSessionContext) {
        strerror = ogs_msprintf("[%s:%d] No AppSessionContext",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    AscReqData = AppSessionContext->asc_req_data;
    if (!AscReqData) {
        strerror = ogs_msprintf("[%s:%d] No AscReqData",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!AscReqData->supp_feat) {
        strerror = ogs_msprintf("[%s:%d] No AscReqData->suppFeat",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!AscReqData->notif_uri) {
        strerror = ogs_msprintf("[%s:%d] No AscReqData->notifUri",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!AscReqData->med_components) {
        strerror = ogs_msprintf("[%s:%d] No AscReqData->MediaCompoenent",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    rc = ogs_sbi_getaddr_from_uri(&scheme, &fqdn, &fqdn_port, &addr, &addr6,
            AscReqData->notif_uri);
    if (rc == false || scheme == OpenAPI_uri_scheme_NULL) {
        strerror = ogs_msprintf("[%s:%d] Invalid URI [%s]",
                pcf_ue_sm->supi, sess->psi, AscReqData->notif_uri);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    supported_features = ogs_uint64_from_string_hexadecimal(
            AscReqData->supp_feat);
    sess->policyauthorization_features &= supported_features;

    if (sess->policyauthorization_features != supported_features) {
        ogs_free(AscReqData->supp_feat);
        AscReqData->supp_feat =
            ogs_uint64_to_string(sess->policyauthorization_features);
        ogs_assert(AscReqData->supp_feat);
    }

    MediaComponentList = AscReqData->med_components;
    OpenAPI_list_for_each(MediaComponentList, node) {
        MediaComponentMap = node->data;
        if (MediaComponentMap) {
            MediaComponent = MediaComponentMap->value;
            if (MediaComponent) {
                if (ims_data.num_of_media_component >=
                        OGS_ARRAY_SIZE(ims_data.media_component)) {
                    ogs_error("OVERFLOW ims_data.num_of_media_component "
                            "[%d:%d:%d]",
                            ims_data.num_of_media_component,
                            OGS_MAX_NUM_OF_MEDIA_COMPONENT,
                            (int)OGS_ARRAY_SIZE(ims_data.media_component));
                    break;
                }
                media_component = &ims_data.
                    media_component[ims_data.num_of_media_component];
                media_component->media_component_number =
                    MediaComponent->med_comp_n;
                media_component->media_type = MediaComponent->med_type;
                if (MediaComponent->mar_bw_dl)
                    media_component->max_requested_bandwidth_dl =
                        ogs_sbi_bitrate_from_string(MediaComponent->mar_bw_dl);
                if (MediaComponent->mar_bw_ul)
                    media_component->max_requested_bandwidth_ul =
                        ogs_sbi_bitrate_from_string(MediaComponent->mar_bw_ul);
                if (MediaComponent->mir_bw_dl)
                    media_component->min_requested_bandwidth_dl =
                        ogs_sbi_bitrate_from_string(MediaComponent->mir_bw_dl);
                if (MediaComponent->mir_bw_ul)
                    media_component->min_requested_bandwidth_ul =
                        ogs_sbi_bitrate_from_string(MediaComponent->mir_bw_ul);
                if (MediaComponent->rr_bw)
                    media_component->rr_bandwidth =
                        ogs_sbi_bitrate_from_string(MediaComponent->rr_bw);
                if (MediaComponent->rs_bw)
                    media_component->rs_bandwidth =
                        ogs_sbi_bitrate_from_string(MediaComponent->rs_bw);
                media_component->flow_status = MediaComponent->f_status;

                SubComponentList = MediaComponent->med_sub_comps;
                OpenAPI_list_for_each(SubComponentList, node2) {
                    if (media_component->num_of_sub >=
                            OGS_ARRAY_SIZE(media_component->sub)) {
                        ogs_error("OVERFLOW media_component->num_of_sub "
                                "[%d:%d:%d]",
                                media_component->num_of_sub,
                                OGS_MAX_NUM_OF_MEDIA_SUB_COMPONENT,
                                (int)OGS_ARRAY_SIZE(media_component->sub));
                        break;
                    }
                    sub = &media_component->sub[media_component->num_of_sub];

                    SubComponentMap = node2->data;
                    if (SubComponentMap) {
                        SubComponent = SubComponentMap->value;
                        if (SubComponent) {
                            sub->flow_number = SubComponent->f_num;
                            sub->flow_usage = SubComponent->flow_usage;

                            fDescList = SubComponent->f_descs;
                            OpenAPI_list_for_each(fDescList, node3) {
                                ogs_flow_t *flow = NULL;

                                if (sub->num_of_flow >=
                                        OGS_ARRAY_SIZE(sub->flow)) {
                                    ogs_error(
                                        "OVERFLOW sub->num_of_flow [%d:%d:%d]",
                                        sub->num_of_flow,
                                        OGS_MAX_NUM_OF_FLOW_IN_MEDIA_SUB_COMPONENT,
                                        (int)OGS_ARRAY_SIZE(sub->flow));
                                    break;
                                }
                                flow = &sub->flow[sub->num_of_flow];
                                if (node3->data) {
                                    flow->description = ogs_strdup(node3->data);
                                    ogs_assert(flow->description);

                                    sub->num_of_flow++;
                                }
                            }
                            media_component->num_of_sub++;
                        }
                    }
                }
                ims_data.num_of_media_component++;
            }
        }
    }

    app_session = pcf_app_add(sess);
    ogs_assert(app_session);

    if (AscReqData->af_app_id) {
        app_session->af_app_id = ogs_strdup(AscReqData->af_app_id);
        ogs_assert(app_session->af_app_id);
    }

    if (app_session->notif_uri)
        ogs_free(app_session->notif_uri);
    app_session->notif_uri = ogs_strdup(AscReqData->notif_uri);
    ogs_assert(app_session->notif_uri);

    client = ogs_sbi_client_find(scheme, fqdn, fqdn_port, addr, addr6);
    if (!client) {
        ogs_debug("%s: ogs_sbi_client_add()", OGS_FUNC);
        client = ogs_sbi_client_add(scheme, fqdn, fqdn_port, addr, addr6);
        if (!client) {
            strerror = ogs_msprintf("%s: ogs_sbi_client_add() failed",
                    OGS_FUNC);
            status = OGS_SBI_HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ogs_freeaddrinfo(addr);
            goto cleanup;
        }
    }
    OGS_SBI_SETUP_CLIENT(&app_session->naf, client);

    ogs_free(fqdn);
    ogs_freeaddrinfo(addr);
    ogs_freeaddrinfo(addr6);

    rv = pcf_get_session_data(
            pcf_ue_sm->supi,
            sess->home.presence == true ? &sess->home.plmn_id : NULL,
            &sess->s_nssai, sess->dnn, &session_data, 0);
    if (rv != OGS_OK) {
        strerror = ogs_msprintf("[%s:%d] Cannot find SUPI in DB",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_NOT_FOUND;
        goto cleanup;
    }

    memset(&SmPolicyDecision, 0, sizeof(SmPolicyDecision));

    PccRuleList = OpenAPI_list_create();
    ogs_assert(PccRuleList);

    QosDecisionList = OpenAPI_list_create();
    ogs_assert(QosDecisionList);

    for (i = 0; i < ims_data.num_of_media_component; i++) {
        int flow_presence = 0;
        ogs_pcc_rule_t fallback_pcc_rule;

        ogs_pcc_rule_t *pcc_rule = NULL;
        ogs_pcc_rule_t *db_pcc_rule = NULL;
        uint8_t qos_index = 0;
        ogs_media_component_t *media_component = &ims_data.media_component[i];

        if (media_component->media_type == OpenAPI_media_type_NULL &&
            (!xcn_qos_override || !xcn_qos_override->qos.index)) {
            strerror = ogs_msprintf("[%s:%d] Media-Type is Required",
                    pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }

        if (xcn_qos_override && xcn_qos_override->qos.index) {
            qos_index = xcn_qos_override->qos.index;
            db_pcc_rule = xcn_qos_override;
        } else {
            switch(media_component->media_type) {
            case OpenAPI_media_type_AUDIO:
                qos_index = OGS_QOS_INDEX_1;
                break;
            case OpenAPI_media_type_VIDEO:
                qos_index = OGS_QOS_INDEX_2;
                break;
            case OpenAPI_media_type_CONTROL:
                qos_index = OGS_QOS_INDEX_5;
                break;
            default:
                strerror = ogs_msprintf("[%s:%d] Unknown Media-Type [%d]",
                        pcf_ue_sm->supi, sess->psi, media_component->media_type);
                status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
                goto cleanup;
            }
        }

        if (!db_pcc_rule) {
            for (j = 0; j < session_data.num_of_pcc_rule; j++) {
                if (session_data.pcc_rule[j].qos.index == qos_index) {
                    db_pcc_rule = &session_data.pcc_rule[j];
                    break;
                }
            }
        }

        if (!db_pcc_rule &&
            (media_component->media_type == OpenAPI_media_type_CONTROL)) {
            /*
             * Check for default bearer for IMS signalling
             * QCI 5 and ARP 1
             */
            if (session_data.session.qos.index != OGS_QOS_INDEX_5 ||
                session_data.session.qos.arp.priority_level != 1) {
                strerror = ogs_msprintf("[%s:%d] CHECK WEBUI : "
                    "Even the Default Bearer(QCI:%d,ARP:%d) "
                    "cannot support IMS signalling.",
                    pcf_ue_sm->supi, sess->psi,
                    session_data.session.qos.index,
                    session_data.session.qos.arp.priority_level);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            } else {
                continue;
            }
        }

        if (!db_pcc_rule && AscReqData->af_app_id &&
            strcmp(AscReqData->af_app_id, XCN_SVC_DEDICATED_BEARER) == 0) {
            memset(&fallback_pcc_rule, 0, sizeof(fallback_pcc_rule));
            fallback_pcc_rule.id = (char *)XCN_SVC_DEDICATED_BEARER;
            fallback_pcc_rule.qos.index = qos_index;
            fallback_pcc_rule.qos.arp.priority_level = 8;
            fallback_pcc_rule.qos.arp.pre_emption_capability =
                OGS_5GC_PRE_EMPTION_DISABLED;
            fallback_pcc_rule.qos.arp.pre_emption_vulnerability =
                OGS_5GC_PRE_EMPTION_ENABLED;
            fallback_pcc_rule.flow_status = media_component->flow_status ?
                media_component->flow_status : OpenAPI_flow_status_ENABLED;
            fallback_pcc_rule.precedence = 100;
            db_pcc_rule = &fallback_pcc_rule;
        }

        if (!db_pcc_rule) {
            strerror = ogs_msprintf("[%s:%d] CHECK WEBUI : "
                "No PCC Rule in DB [QoS Index:%d] - "
                "Please add PCC Rule using WEBUI",
                pcf_ue_sm->supi, sess->psi, qos_index);
            status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
            goto cleanup;
        }

        for (j = 0; j < app_session->num_of_pcc_rule; j++) {
            if (app_session->pcc_rule[j].qos.index == qos_index) {
                pcc_rule = &app_session->pcc_rule[j];
                break;
            }
        }

        if (!pcc_rule) {
            pcc_rule = &app_session->pcc_rule[app_session->num_of_pcc_rule];
            ogs_assert(pcc_rule);

            pcc_rule->id = ogs_msprintf("%s-a%s",
                            db_pcc_rule->id, app_session->app_session_id);
            ogs_assert(pcc_rule->id);

            memcpy(&pcc_rule->qos, &db_pcc_rule->qos, sizeof(ogs_qos_t));

            pcc_rule->flow_status = db_pcc_rule->flow_status;
            pcc_rule->precedence = db_pcc_rule->precedence;

            /* Install Flow */
            flow_presence = 1;
            rv = ogs_pcc_rule_install_flow_from_media(
                    pcc_rule, media_component);
            if (rv != OGS_OK) {
                strerror = ogs_msprintf("[%s:%d] install_flow() failed",
                    pcf_ue_sm->supi, sess->psi);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            }

            app_session->num_of_pcc_rule++;

        } else {
            int count = 0;

            /* Check Flow */
            count = ogs_pcc_rule_num_of_flow_equal_to_media(
                    pcc_rule, media_component);
            if (count == -1) {
                strerror = ogs_msprintf("[%s:%d] matched_flow() failed",
                    pcf_ue_sm->supi, sess->psi);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            }

            if (pcc_rule->num_of_flow != count) {
                /* Re-install Flow */
                flow_presence = 1;
                rv = ogs_pcc_rule_install_flow_from_media(
                        pcc_rule, media_component);
                if (rv != OGS_OK) {
                    strerror = ogs_msprintf("[%s:%d] re-install_flow() failed",
                        pcf_ue_sm->supi, sess->psi);
                    status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                    goto cleanup;
                }
            }
        }

        /* Update QoS */
        rv = ogs_pcc_rule_update_qos_from_media(pcc_rule, media_component);
        if (rv != OGS_OK) {
            strerror = ogs_msprintf("[%s:%d] update_qos() failed",
                pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
            goto cleanup;
        }

        /* if we failed to get QoS from IMS, apply WEBUI QoS */
        if (pcc_rule->qos.mbr.downlink == 0)
            pcc_rule->qos.mbr.downlink = db_pcc_rule->qos.mbr.downlink;
        if (pcc_rule->qos.mbr.uplink == 0)
            pcc_rule->qos.mbr.uplink = db_pcc_rule->qos.mbr.uplink;
        if (pcc_rule->qos.gbr.downlink == 0)
            pcc_rule->qos.gbr.downlink = db_pcc_rule->qos.gbr.downlink;
        if (pcc_rule->qos.gbr.uplink == 0)
            pcc_rule->qos.gbr.uplink = db_pcc_rule->qos.gbr.uplink;

        /**************************************************************
         * Build PCC Rule & QoS Decision
         *************************************************************/
        PccRule = ogs_sbi_build_pcc_rule(pcc_rule, flow_presence);
        ogs_assert(PccRule->pcc_rule_id);

        PccRuleMap = OpenAPI_map_create(PccRule->pcc_rule_id, PccRule);
        ogs_assert(PccRuleMap);

        OpenAPI_list_add(PccRuleList, PccRuleMap);

        QosData = ogs_sbi_build_qos_data(pcc_rule);
        ogs_assert(QosData);
        ogs_assert(QosData->qos_id);

        QosDecisionMap = OpenAPI_map_create(QosData->qos_id, QosData);
        ogs_assert(QosDecisionMap);

        OpenAPI_list_add(QosDecisionList, QosDecisionMap);
    }

    pcf_free_policy_decision_lists(&PccRuleList, &QosDecisionList);

    if (pcf_build_app_policy_decision(
                sess, &SmPolicyDecision, &PccRuleList, &QosDecisionList)) {
        if (pcf_sbi_send_smpolicycontrol_update_notify(
                    sess, &SmPolicyDecision) != true) {
            strerror = ogs_msprintf("[%s:%d] SM policy update notify failed",
                    pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_GATEWAY_TIMEOUT;
            goto cleanup;
        }
    } else {
        ogs_warn("[%s:%d] Empty app policy decision [apps:%d]",
                pcf_ue_sm->supi, sess->psi,
                ogs_list_count(&sess->app_list));
    }

    memset(&sendmsg, 0, sizeof(sendmsg));

    memset(&header, 0, sizeof(header));
    header.service.name = (char *)OGS_SBI_SERVICE_NAME_NPCF_POLICYAUTHORIZATION;
    header.api.version = (char *)OGS_SBI_API_V1;
    header.resource.component[0] = (char *)OGS_SBI_RESOURCE_NAME_APP_SESSIONS;
    header.resource.component[1] = (char *)app_session->app_session_id;
    sendmsg.http.location = ogs_sbi_server_uri(server, &header);
    ogs_assert(sendmsg.http.location);

    sendmsg.AppSessionContext = recvmsg->AppSessionContext;

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_CREATED);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    ogs_free(sendmsg.http.location);

    pcf_free_policy_decision_lists(&PccRuleList, &QosDecisionList);

    ogs_ims_data_free(&ims_data);
    OGS_SESSION_DATA_FREE(&session_data);

    return true;

cleanup:
    ogs_assert(status);
    ogs_assert(strerror);
    ogs_error("%s", strerror);
    ogs_assert(true ==
        ogs_sbi_server_send_error(stream, status, recvmsg, strerror, NULL,
                NULL));
    ogs_free(strerror);

    if (app_session)
        pcf_app_remove(app_session);

    pcf_free_policy_decision_lists(&PccRuleList, &QosDecisionList);

    ogs_ims_data_free(&ims_data);
    OGS_SESSION_DATA_FREE(&session_data);

    return false;
}

bool pcf_npcf_policyauthorization_handle_update(
        pcf_sess_t *sess, pcf_app_t *app_session,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    int i, j, rv, status = 0;
    char *strerror = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    OpenAPI_app_session_context_update_data_patch_t
        *AppSessionContextUpdateDataPatch = NULL;
    OpenAPI_app_session_context_update_data_t *AscUpdateData = NULL;

    ogs_sbi_message_t sendmsg;
    ogs_sbi_response_t *response = NULL;

    ogs_session_data_t session_data;

    ogs_ims_data_t ims_data;
    ogs_media_component_t *media_component = NULL;
    ogs_media_sub_component_t *sub = NULL;

    OpenAPI_list_t *MediaComponentList = NULL;
    OpenAPI_map_t *MediaComponentMap = NULL;
    OpenAPI_media_component_rm_t *MediaComponent = NULL;

    OpenAPI_list_t *SubComponentList = NULL;
    OpenAPI_map_t *SubComponentMap = NULL;
    OpenAPI_media_sub_component_rm_t *SubComponent = NULL;

    OpenAPI_list_t *fDescList = NULL;

    OpenAPI_sm_policy_decision_t SmPolicyDecision;

    OpenAPI_list_t *PccRuleList = NULL;
    OpenAPI_map_t *PccRuleMap = NULL;
    OpenAPI_pcc_rule_t *PccRule = NULL;

    OpenAPI_list_t *QosDecisionList = NULL;
    OpenAPI_map_t *QosDecisionMap = NULL;
    OpenAPI_qos_data_t *QosData = NULL;

    OpenAPI_lnode_t *node = NULL, *node2 = NULL, *node3 = NULL;

    ogs_assert(sess);
    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(app_session);
    ogs_assert(stream);
    ogs_assert(recvmsg);

    memset(&ims_data, 0, sizeof(ims_data));
    memset(&session_data, 0, sizeof(ogs_session_data_t));

    AppSessionContextUpdateDataPatch =
        recvmsg->AppSessionContextUpdateDataPatch;
    if (!AppSessionContextUpdateDataPatch) {
        strerror = ogs_msprintf("[%s:%d] No AppSessionContextUpdateDataPatch",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    AscUpdateData = AppSessionContextUpdateDataPatch->asc_req_data;
    if (!AscUpdateData) {
        strerror = ogs_msprintf("[%s:%d] No AscUpdateData",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    if (!AscUpdateData->med_components) {
        strerror = ogs_msprintf("[%s:%d] No AscUpdateData->MediaCompoenent",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
        goto cleanup;
    }

    MediaComponentList = AscUpdateData->med_components;
    OpenAPI_list_for_each(MediaComponentList, node) {
        MediaComponentMap = node->data;
        if (MediaComponentMap) {
            MediaComponent = MediaComponentMap->value;
            if (MediaComponent) {
                if (ims_data.num_of_media_component >=
                        OGS_ARRAY_SIZE(ims_data.media_component)) {
                    ogs_error("OVERFLOW ims_data.num_of_media_component "
                            "[%d:%d:%d]",
                            ims_data.num_of_media_component,
                            OGS_MAX_NUM_OF_MEDIA_COMPONENT,
                            (int)OGS_ARRAY_SIZE(ims_data.media_component));
                    break;
                }
                media_component = &ims_data.
                    media_component[ims_data.num_of_media_component];

                media_component->media_component_number =
                    MediaComponent->med_comp_n;
                media_component->media_type = MediaComponent->med_type;
                if (MediaComponent->mar_bw_dl)
                    media_component->max_requested_bandwidth_dl =
                        ogs_sbi_bitrate_from_string(MediaComponent->mar_bw_dl);
                if (MediaComponent->mar_bw_ul)
                    media_component->max_requested_bandwidth_ul =
                        ogs_sbi_bitrate_from_string(MediaComponent->mar_bw_ul);
                if (MediaComponent->mir_bw_dl)
                    media_component->min_requested_bandwidth_dl =
                        ogs_sbi_bitrate_from_string(MediaComponent->mir_bw_dl);
                if (MediaComponent->mir_bw_ul)
                    media_component->min_requested_bandwidth_ul =
                        ogs_sbi_bitrate_from_string(MediaComponent->mir_bw_ul);
                if (MediaComponent->rr_bw)
                    media_component->rr_bandwidth =
                        ogs_sbi_bitrate_from_string(MediaComponent->rr_bw);
                if (MediaComponent->rs_bw)
                    media_component->rs_bandwidth =
                        ogs_sbi_bitrate_from_string(MediaComponent->rs_bw);
                media_component->flow_status = MediaComponent->f_status;

                SubComponentList = MediaComponent->med_sub_comps;
                OpenAPI_list_for_each(SubComponentList, node2) {
                    if (media_component->num_of_sub >=
                            OGS_ARRAY_SIZE(media_component->sub)) {
                        ogs_error("OVERFLOW media_component->num_of_sub "
                                "[%d:%d:%d]",
                                media_component->num_of_sub,
                                OGS_MAX_NUM_OF_MEDIA_SUB_COMPONENT,
                                (int)OGS_ARRAY_SIZE(media_component->sub));
                        break;
                    }
                    sub = &media_component->sub[media_component->num_of_sub];

                    SubComponentMap = node2->data;
                    if (SubComponentMap) {
                        SubComponent = SubComponentMap->value;
                        if (SubComponent) {
                            sub->flow_number = SubComponent->f_num;
                            sub->flow_usage = SubComponent->flow_usage;

                            fDescList = SubComponent->f_descs;
                            OpenAPI_list_for_each(fDescList, node3) {
                                ogs_flow_t *flow = NULL;

                                if (sub->num_of_flow >=
                                        OGS_ARRAY_SIZE(sub->flow)) {
                                    ogs_error(
                                        "OVERFLOW sub->num_of_flow [%d:%d:%d]",
                                        sub->num_of_flow,
                                        OGS_MAX_NUM_OF_FLOW_IN_MEDIA_SUB_COMPONENT,
                                        (int)OGS_ARRAY_SIZE(sub->flow));
                                    break;
                                }
                                flow = &sub->flow[sub->num_of_flow];
                                if (node3->data) {
                                    flow->description = ogs_strdup(node3->data);
                                    ogs_assert(flow->description);

                                    sub->num_of_flow++;
                                }
                            }
                            media_component->num_of_sub++;
                        }
                    }
                }
                ims_data.num_of_media_component++;
            }
        }
    }

    rv = pcf_get_session_data(
            pcf_ue_sm->supi,
            sess->home.presence == true ? &sess->home.plmn_id : NULL,
            &sess->s_nssai, sess->dnn, &session_data, 0);
    if (rv != OGS_OK) {
        strerror = ogs_msprintf("[%s:%d] Cannot find SUPI in DB",
                pcf_ue_sm->supi, sess->psi);
        status = OGS_SBI_HTTP_STATUS_NOT_FOUND;
        goto cleanup;
    }

    memset(&SmPolicyDecision, 0, sizeof(SmPolicyDecision));

    PccRuleList = OpenAPI_list_create();
    ogs_assert(PccRuleList);

    QosDecisionList = OpenAPI_list_create();
    ogs_assert(QosDecisionList);

    for (i = 0; i < ims_data.num_of_media_component; i++) {
        int flow_presence = 0;

        ogs_pcc_rule_t *pcc_rule = NULL;
        ogs_pcc_rule_t *db_pcc_rule = NULL;
        uint8_t qos_index = 0;
        ogs_media_component_t *media_component = &ims_data.media_component[i];

        if (media_component->media_type == OpenAPI_media_type_NULL) {
            strerror = ogs_msprintf("[%s:%d] Media-Type is Required",
                    pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }

        switch(media_component->media_type) {
        case OpenAPI_media_type_AUDIO:
            qos_index = OGS_QOS_INDEX_1;
            break;
        case OpenAPI_media_type_VIDEO:
            qos_index = OGS_QOS_INDEX_2;
            break;
        case OpenAPI_media_type_CONTROL:
            qos_index = OGS_QOS_INDEX_5;
            break;
        default:
            strerror = ogs_msprintf("[%s:%d] Unknown Media-Type [%d]",
                    pcf_ue_sm->supi, sess->psi, media_component->media_type);
            status = OGS_SBI_HTTP_STATUS_BAD_REQUEST;
            goto cleanup;
        }

        for (j = 0; j < session_data.num_of_pcc_rule; j++) {
            if (session_data.pcc_rule[j].qos.index == qos_index) {
                db_pcc_rule = &session_data.pcc_rule[j];
                break;
            }
        }

        if (!db_pcc_rule &&
            (media_component->media_type == OpenAPI_media_type_CONTROL)) {
            /*
             * Check for default bearer for IMS signalling
             * QCI 5 and ARP 1
             */
            if (session_data.session.qos.index != OGS_QOS_INDEX_5 ||
                session_data.session.qos.arp.priority_level != 1) {
                strerror = ogs_msprintf("[%s:%d] CHECK WEBUI : "
                    "Even the Default Bearer(QCI:%d,ARP:%d) "
                    "cannot support IMS signalling.",
                    pcf_ue_sm->supi, sess->psi,
                    session_data.session.qos.index,
                    session_data.session.qos.arp.priority_level);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            } else {
                continue;
            }
        }

        if (!db_pcc_rule) {
            strerror = ogs_msprintf("[%s:%d] CHECK WEBUI : "
                "No PCC Rule in DB [QoS Index:%d] - "
                "Please add PCC Rule using WEBUI",
                pcf_ue_sm->supi, sess->psi, qos_index);
            status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
            goto cleanup;
        }

        for (j = 0; j < app_session->num_of_pcc_rule; j++) {
            if (app_session->pcc_rule[j].qos.index == qos_index) {
                pcc_rule = &app_session->pcc_rule[j];
                break;
            }
        }

        if (!pcc_rule) {
            pcc_rule = &app_session->pcc_rule[app_session->num_of_pcc_rule];
            ogs_assert(pcc_rule);

            pcc_rule->id = ogs_strdup(app_session->app_session_id);
            ogs_assert(pcc_rule->id);

            memcpy(&pcc_rule->qos, &db_pcc_rule->qos, sizeof(ogs_qos_t));

            pcc_rule->flow_status = db_pcc_rule->flow_status;
            pcc_rule->precedence = db_pcc_rule->precedence;

            /* Install Flow */
            flow_presence = 1;
            rv = ogs_pcc_rule_install_flow_from_media(
                    pcc_rule, media_component);
            if (rv != OGS_OK) {
                strerror = ogs_msprintf("[%s:%d] install_flow() failed",
                    pcf_ue_sm->supi, sess->psi);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            }

            app_session->num_of_pcc_rule++;

        } else {
            int count = 0;

            /* Check Flow */
            count = ogs_pcc_rule_num_of_flow_equal_to_media(
                    pcc_rule, media_component);
            if (count == -1) {
                strerror = ogs_msprintf("[%s:%d] matched_flow() failed",
                    pcf_ue_sm->supi, sess->psi);
                status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                goto cleanup;
            }

            if (pcc_rule->num_of_flow != count) {
                /* Re-install Flow */
                flow_presence = 1;
                rv = ogs_pcc_rule_install_flow_from_media(
                        pcc_rule, media_component);
                if (rv != OGS_OK) {
                    strerror = ogs_msprintf("[%s:%d] re-install_flow() failed",
                        pcf_ue_sm->supi, sess->psi);
                    status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
                    goto cleanup;
                }
            }
        }

        /* Update QoS */
        rv = ogs_pcc_rule_update_qos_from_media(pcc_rule, media_component);
        if (rv != OGS_OK) {
            strerror = ogs_msprintf("[%s:%d] update_qos() failed",
                pcf_ue_sm->supi, sess->psi);
            status = OGS_SBI_HTTP_STATUS_FORBIDDEN;
            goto cleanup;
        }

        /* if we failed to get QoS from IMS, apply WEBUI QoS */
        if (pcc_rule->qos.mbr.downlink == 0)
            pcc_rule->qos.mbr.downlink = db_pcc_rule->qos.mbr.downlink;
        if (pcc_rule->qos.mbr.uplink == 0)
            pcc_rule->qos.mbr.uplink = db_pcc_rule->qos.mbr.uplink;
        if (pcc_rule->qos.gbr.downlink == 0)
            pcc_rule->qos.gbr.downlink = db_pcc_rule->qos.gbr.downlink;
        if (pcc_rule->qos.gbr.uplink == 0)
            pcc_rule->qos.gbr.uplink = db_pcc_rule->qos.gbr.uplink;

        /**************************************************************
         * Build PCC Rule & QoS Decision
         *************************************************************/
        PccRule = ogs_sbi_build_pcc_rule(pcc_rule, flow_presence);
        ogs_assert(PccRule->pcc_rule_id);

        PccRuleMap = OpenAPI_map_create(PccRule->pcc_rule_id, PccRule);
        ogs_assert(PccRuleMap);

        OpenAPI_list_add(PccRuleList, PccRuleMap);

        QosData = ogs_sbi_build_qos_data(pcc_rule);
        ogs_assert(QosData);
        ogs_assert(QosData->qos_id);

        QosDecisionMap = OpenAPI_map_create(QosData->qos_id, QosData);
        ogs_assert(QosDecisionMap);

        OpenAPI_list_add(QosDecisionList, QosDecisionMap);
    }

    if (PccRuleList->count)
        SmPolicyDecision.pcc_rules = PccRuleList;

    if (QosDecisionList->count)
        SmPolicyDecision.qos_decs = QosDecisionList;

    memset(&sendmsg, 0, sizeof(sendmsg));

    sendmsg.AppSessionContextUpdateDataPatch =
        recvmsg->AppSessionContextUpdateDataPatch;

    response = ogs_sbi_build_response(&sendmsg, OGS_SBI_HTTP_STATUS_OK);
    ogs_assert(response);
    ogs_assert(true == ogs_sbi_server_send_response(stream, response));

    if (PccRuleList->count || QosDecisionList->count) {
        ogs_assert(true == pcf_sbi_send_smpolicycontrol_update_notify(
                            sess, &SmPolicyDecision));
    }

    OpenAPI_list_for_each(PccRuleList, node) {
        PccRuleMap = node->data;
        if (PccRuleMap) {
            PccRule = PccRuleMap->value;
            if (PccRule)
                ogs_sbi_free_pcc_rule(PccRule);
            ogs_free(PccRuleMap);
        }
    }
    OpenAPI_list_free(PccRuleList);

    OpenAPI_list_for_each(QosDecisionList, node) {
        QosDecisionMap = node->data;
        if (QosDecisionMap) {
            QosData = QosDecisionMap->value;
            if (QosData)
                ogs_sbi_free_qos_data(QosData);
            ogs_free(QosDecisionMap);
        }
    }
    OpenAPI_list_free(QosDecisionList);

    ogs_ims_data_free(&ims_data);
    OGS_SESSION_DATA_FREE(&session_data);

    return true;

cleanup:
    ogs_assert(status);
    ogs_assert(strerror);
    ogs_error("%s", strerror);
    ogs_assert(true ==
        ogs_sbi_server_send_error(stream, status, recvmsg, strerror,
                NULL, NULL));
    ogs_free(strerror);

    OpenAPI_list_for_each(PccRuleList, node) {
        PccRuleMap = node->data;
        if (PccRuleMap) {
            PccRule = PccRuleMap->value;
            if (PccRule)
                ogs_sbi_free_pcc_rule(PccRule);
            ogs_free(PccRuleMap);
        }
    }
    OpenAPI_list_free(PccRuleList);

    OpenAPI_list_for_each(QosDecisionList, node) {
        QosDecisionMap = node->data;
        if (QosDecisionMap) {
            QosData = QosDecisionMap->value;
            if (QosData)
                ogs_sbi_free_qos_data(QosData);
            ogs_free(QosDecisionMap);
        }
    }
    OpenAPI_list_free(QosDecisionList);

    ogs_ims_data_free(&ims_data);
    OGS_SESSION_DATA_FREE(&session_data);

    return false;
}

bool pcf_npcf_policyauthorization_handle_delete(
        pcf_sess_t *sess, pcf_app_t *app_session,
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    int i;

    OpenAPI_sm_policy_decision_t SmPolicyDecision;

    OpenAPI_list_t *PccRuleList = NULL;
    OpenAPI_map_t *PccRuleMap = NULL;

    OpenAPI_list_t *QosDecisionList = NULL;
    OpenAPI_map_t *QosDecisionMap = NULL;

    OpenAPI_lnode_t *node = NULL;

    ogs_assert(true == ogs_sbi_send_http_status_no_content(stream));

    ogs_assert(app_session);

    memset(&SmPolicyDecision, 0, sizeof(SmPolicyDecision));

    PccRuleList = OpenAPI_list_create();
    ogs_assert(PccRuleList);

    QosDecisionList = OpenAPI_list_create();
    ogs_assert(QosDecisionList);

    for (i = 0; i < app_session->num_of_pcc_rule; i++) {
        ogs_pcc_rule_t *pcc_rule = &app_session->pcc_rule[i];

        ogs_assert(pcc_rule);

        PccRuleMap = OpenAPI_map_create(pcc_rule->id, NULL);
        ogs_assert(PccRuleMap);

        OpenAPI_list_add(PccRuleList, PccRuleMap);

        QosDecisionMap = OpenAPI_map_create(pcc_rule->id, NULL);
        ogs_assert(QosDecisionMap);

        OpenAPI_list_add(QosDecisionList, QosDecisionMap);
    }

    if (PccRuleList->count)
        SmPolicyDecision.pcc_rules = PccRuleList;

    if (QosDecisionList->count)
        SmPolicyDecision.qos_decs = QosDecisionList;

    if (PccRuleList->count || QosDecisionList->count) {
        ogs_assert(true == pcf_sbi_send_smpolicycontrol_delete_notify(
                            sess, app_session, &SmPolicyDecision));
    } else {
        pcf_app_remove(app_session);
    }

    OpenAPI_list_for_each(PccRuleList, node) {
        PccRuleMap = node->data;
        if (PccRuleMap) {
            ogs_free(PccRuleMap);
        }
    }
    OpenAPI_list_free(PccRuleList);

    OpenAPI_list_for_each(QosDecisionList, node) {
        QosDecisionMap = node->data;
        if (QosDecisionMap) {
            ogs_free(QosDecisionMap);
        }
    }
    OpenAPI_list_free(QosDecisionList);

    return true;
}

bool pcf_xcn_dedicated_bearer_handle_create(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg,
        const char *content)
{
    cJSON *item = NULL, *flow_descriptions = NULL, *flow = NULL;
    ogs_pcc_rule_t xcn_qos_override;
    bool has_xcn_qos_override = false;
    pcf_sess_t *sess = NULL;
    OpenAPI_app_session_context_t *app_context = NULL;
    OpenAPI_app_session_context_req_data_t *asc_req_data = NULL;
    OpenAPI_media_component_t *media_component = NULL;
    OpenAPI_media_sub_component_t *sub_component = NULL;
    OpenAPI_map_t *media_map = NULL, *sub_map = NULL;
    const char *notif_uri = NULL;
    const char *error_detail = NULL;
    OpenAPI_media_type_e media_type = OpenAPI_media_type_NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!content)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No request body");

    item = cJSON_Parse(content);
    if (!item)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "Invalid JSON body");

    if (!xcn_parse_bearer_request(item, &sess)) {
        cJSON_Delete(item);
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_NOT_FOUND,
                "No PCF SM policy session for target UE");
    }
    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(pcf_ue_sm);

    memset(&xcn_qos_override, 0, sizeof(xcn_qos_override));
    if (!xcn_parse_qos_override(item, &xcn_qos_override, &error_detail)) {
        cJSON_Delete(item);
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, error_detail);
    }
    has_xcn_qos_override = xcn_qos_override.qos.index ? true : false;

    media_type = xcn_media_type_from_json(item);
    if (media_type == OpenAPI_media_type_NULL) {
        if (!has_xcn_qos_override) {
            cJSON_Delete(item);
            return xcn_send_error(stream, recvmsg,
                    OGS_SBI_HTTP_STATUS_BAD_REQUEST, "Invalid mediaType");
        }
        media_type = OpenAPI_media_type_DATA;
    }

    flow_descriptions =
        cJSON_GetObjectItemCaseSensitive(item, "flowDescriptions");
    if (!cJSON_IsArray(flow_descriptions) ||
        cJSON_GetArraySize(flow_descriptions) == 0) {
        cJSON_Delete(item);
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No flowDescriptions");
    }

    app_context = ogs_calloc(1, sizeof(*app_context));
    ogs_assert(app_context);
    asc_req_data = ogs_calloc(1, sizeof(*asc_req_data));
    ogs_assert(asc_req_data);
    media_component = ogs_calloc(1, sizeof(*media_component));
    ogs_assert(media_component);
    sub_component = ogs_calloc(1, sizeof(*sub_component));
    ogs_assert(sub_component);

    app_context->asc_req_data = asc_req_data;
    asc_req_data->af_app_id = ogs_strdup(XCN_SVC_DEDICATED_BEARER);
    ogs_assert(asc_req_data->af_app_id);
    asc_req_data->supp_feat = ogs_strdup("0");
    ogs_assert(asc_req_data->supp_feat);

    notif_uri = xcn_json_string(item, "notificationUri");
    asc_req_data->notif_uri = notif_uri ?
        ogs_strdup(notif_uri) :
        ogs_strdup("http://127.0.0.1:7785/xcn-dedicated-bearer/v1/notifications");
    ogs_assert(asc_req_data->notif_uri);

    asc_req_data->supi = ogs_strdup(pcf_ue_sm->supi);
    ogs_assert(asc_req_data->supi);
    if (sess->dnn)
        asc_req_data->dnn = ogs_strdup(sess->dnn);

    media_component->med_comp_n = 0;
    media_component->f_status = OpenAPI_flow_status_ENABLED;
    media_component->med_type = media_type;

#define XCN_COPY_BW(__json_key, __field) \
    do { \
        const char *__v = xcn_json_string(item, (__json_key)); \
        if (__v) { \
            media_component->__field = ogs_strdup(__v); \
            ogs_assert(media_component->__field); \
        } \
    } while (0)
    XCN_COPY_BW("marBwDl", mar_bw_dl);
    XCN_COPY_BW("marBwUl", mar_bw_ul);
    XCN_COPY_BW("mirBwDl", mir_bw_dl);
    XCN_COPY_BW("mirBwUl", mir_bw_ul);
    XCN_COPY_BW("rrBw", rr_bw);
    XCN_COPY_BW("rsBw", rs_bw);
#undef XCN_COPY_BW

#define XCN_COPY_QOS_BW(__json_key, __field) \
    do { \
        cJSON *__qos = cJSON_GetObjectItemCaseSensitive(item, "qos"); \
        const char *__v = cJSON_IsObject(__qos) ? \
            xcn_json_string(__qos, (__json_key)) : NULL; \
        if (__v) { \
            if (media_component->__field) \
                ogs_free(media_component->__field); \
            media_component->__field = ogs_strdup(__v); \
            ogs_assert(media_component->__field); \
        } \
    } while (0)
    XCN_COPY_QOS_BW("maxbrDl", mar_bw_dl);
    XCN_COPY_QOS_BW("mbrDl", mar_bw_dl);
    XCN_COPY_QOS_BW("maxbrUl", mar_bw_ul);
    XCN_COPY_QOS_BW("mbrUl", mar_bw_ul);
    XCN_COPY_QOS_BW("gbrDl", mir_bw_dl);
    XCN_COPY_QOS_BW("gbrUl", mir_bw_ul);
#undef XCN_COPY_QOS_BW

    sub_component->f_num = 0;
    sub_component->flow_usage = OpenAPI_flow_usage_NO_INFO;
    sub_component->f_descs = OpenAPI_list_create();
    ogs_assert(sub_component->f_descs);

    cJSON_ArrayForEach(flow, flow_descriptions) {
        if (cJSON_IsString(flow) && flow->valuestring) {
            OpenAPI_list_add(sub_component->f_descs,
                    ogs_strdup(flow->valuestring));
        }
    }

    if (sub_component->f_descs->count == 0) {
        recvmsg->AppSessionContext = app_context;
        cJSON_Delete(item);
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No valid flowDescriptions");
    }

    media_component->med_sub_comps = OpenAPI_list_create();
    ogs_assert(media_component->med_sub_comps);
    sub_map = OpenAPI_map_create(ogs_msprintf("%d", sub_component->f_num),
            sub_component);
    ogs_assert(sub_map);
    OpenAPI_list_add(media_component->med_sub_comps, sub_map);

    asc_req_data->med_components = OpenAPI_list_create();
    ogs_assert(asc_req_data->med_components);
    media_map = OpenAPI_map_create(
            ogs_msprintf("%d", media_component->med_comp_n), media_component);
    ogs_assert(media_map);
    OpenAPI_list_add(asc_req_data->med_components, media_map);

    recvmsg->AppSessionContext = app_context;
    cJSON_Delete(item);

    return pcf_npcf_policyauthorization_handle_create(
            sess, stream, recvmsg,
            has_xcn_qos_override ? &xcn_qos_override : NULL);
}

static bool xcn_app_is_dedicated_bearer(pcf_app_t *app)
{
    int i;

    ogs_assert(app);

    if (app->af_app_id &&
        strcmp(app->af_app_id, XCN_SVC_DEDICATED_BEARER) == 0)
        return true;

    for (i = 0; i < app->num_of_pcc_rule; i++) {
        ogs_pcc_rule_t *pcc_rule = &app->pcc_rule[i];

        if (pcc_rule->id &&
            !strncmp(pcc_rule->id, XCN_SVC_DEDICATED_BEARER,
                    strlen(XCN_SVC_DEDICATED_BEARER)))
            return true;
    }

    return false;
}

static const char *xcn_flow_direction_to_string(uint8_t direction)
{
    switch (direction) {
    case OGS_FLOW_DOWNLINK_ONLY:
        return "DOWNLINK";
    case OGS_FLOW_UPLINK_ONLY:
        return "UPLINK";
    case OGS_FLOW_BIDIRECTIONAL:
        return "BIDIRECTIONAL";
    default:
        return "UNSPECIFIED";
    }
}

static cJSON *xcn_pcc_rule_to_json(ogs_pcc_rule_t *pcc_rule)
{
    int i;
    cJSON *item = NULL, *qos = NULL, *arp = NULL, *flows = NULL;

    ogs_assert(pcc_rule);

    item = cJSON_CreateObject();
    ogs_assert(item);

    if (pcc_rule->id)
        cJSON_AddStringToObject(item, "pccRuleId", pcc_rule->id);
    cJSON_AddNumberToObject(item, "precedence", pcc_rule->precedence);
    cJSON_AddStringToObject(item, "flowStatus",
            OpenAPI_flow_status_ToString(pcc_rule->flow_status));

    qos = cJSON_AddObjectToObject(item, "qos");
    ogs_assert(qos);
    cJSON_AddNumberToObject(qos, "5qi", pcc_rule->qos.index);

    arp = cJSON_AddObjectToObject(qos, "arp");
    ogs_assert(arp);
    cJSON_AddNumberToObject(arp, "priorityLevel",
            pcc_rule->qos.arp.priority_level);
    cJSON_AddNumberToObject(arp, "preemptionCapability",
            pcc_rule->qos.arp.pre_emption_capability);
    cJSON_AddNumberToObject(arp, "preemptionVulnerability",
            pcc_rule->qos.arp.pre_emption_vulnerability);

    if (pcc_rule->qos.mbr.downlink)
        cJSON_AddNumberToObject(qos, "mbrDl", pcc_rule->qos.mbr.downlink);
    if (pcc_rule->qos.mbr.uplink)
        cJSON_AddNumberToObject(qos, "mbrUl", pcc_rule->qos.mbr.uplink);
    if (pcc_rule->qos.gbr.downlink)
        cJSON_AddNumberToObject(qos, "gbrDl", pcc_rule->qos.gbr.downlink);
    if (pcc_rule->qos.gbr.uplink)
        cJSON_AddNumberToObject(qos, "gbrUl", pcc_rule->qos.gbr.uplink);

    flows = cJSON_AddArrayToObject(item, "flows");
    ogs_assert(flows);
    for (i = 0; i < pcc_rule->num_of_flow; i++) {
        cJSON *flow = cJSON_CreateObject();
        ogs_assert(flow);

        cJSON_AddStringToObject(flow, "direction",
                xcn_flow_direction_to_string(pcc_rule->flow[i].direction));
        if (pcc_rule->flow[i].description)
            cJSON_AddStringToObject(flow, "description",
                    pcc_rule->flow[i].description);
        cJSON_AddItemToArray(flows, flow);
    }

    return item;
}

static cJSON *xcn_app_to_json(pcf_app_t *app)
{
    int i;
    cJSON *item = NULL, *pcc_rules = NULL;

    ogs_assert(app);

    item = cJSON_CreateObject();
    ogs_assert(item);

    if (app->app_session_id)
        cJSON_AddStringToObject(item, "appSessionId", app->app_session_id);
    if (app->af_app_id)
        cJSON_AddStringToObject(item, "afAppId", app->af_app_id);

    pcc_rules = cJSON_AddArrayToObject(item, "pccRules");
    ogs_assert(pcc_rules);
    for (i = 0; i < app->num_of_pcc_rule; i++)
        cJSON_AddItemToArray(pcc_rules,
                xcn_pcc_rule_to_json(&app->pcc_rule[i]));
    cJSON_AddNumberToObject(item, "pccRuleCount", app->num_of_pcc_rule);

    return item;
}

bool pcf_xcn_dedicated_bearer_handle_query(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg,
        ogs_hash_t *params)
{
    cJSON *root = NULL, *bearers = NULL;
    pcf_sess_t *sess = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    pcf_app_t *app = NULL;
    int bearer_count = 0;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!xcn_parse_bearer_params(params, &sess))
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_NOT_FOUND,
                "No PCF SM policy session for target UE");

    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(pcf_ue_sm);

    root = cJSON_CreateObject();
    ogs_assert(root);

    cJSON_AddStringToObject(root, "supi", pcf_ue_sm->supi);
    cJSON_AddStringToObject(root, "imsi", xcn_supi_to_imsi(pcf_ue_sm->supi));
    cJSON_AddNumberToObject(root, "pduSessionId", sess->psi);
    if (sess->ipv4addr_string)
        cJSON_AddStringToObject(root, "ueIp", sess->ipv4addr_string);
    else if (sess->ipv6prefix_string)
        cJSON_AddStringToObject(root, "ueIp", sess->ipv6prefix_string);
    if (sess->amf_ue_ngap_id)
        cJSON_AddNumberToObject(root, "amfUeNgapId", sess->amf_ue_ngap_id);
    if (sess->ran_ue_ngap_id)
        cJSON_AddNumberToObject(root, "ranUeNgapId", sess->ran_ue_ngap_id);

    bearers = cJSON_AddArrayToObject(root, "bearers");
    ogs_assert(bearers);

    ogs_list_for_each(&sess->app_list, app) {
        if (!xcn_app_is_dedicated_bearer(app))
            continue;

        cJSON_AddItemToArray(bearers, xcn_app_to_json(app));
        bearer_count++;
    }
    cJSON_AddNumberToObject(root, "bearerCount", bearer_count);

    if (xcn_send_json_response(stream, OGS_SBI_HTTP_STATUS_OK, root) == false) {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

bool pcf_xcn_dedicated_bearer_handle_delete(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    pcf_app_t *app_session = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!recvmsg->h.resource.component[1])
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No appSessionId");

    app_session = pcf_app_find_by_app_session_id(
            recvmsg->h.resource.component[1]);
    if (!app_session)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_NOT_FOUND, "No appSessionId");

    return pcf_npcf_policyauthorization_handle_delete(
            app_session->sess, app_session, stream, recvmsg);
}

bool pcf_xcn_query_handle_users(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg,
        ogs_hash_t *params)
{
    cJSON *root = NULL, *users = NULL;
    pcf_context_t *self = pcf_self();
    pcf_ue_am_t *pcf_ue_am = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    const char *tmsi_string = NULL;
    char *tmsi_supi = NULL;
    int user_count = 0;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    tmsi_string = params ? ogs_sbi_header_get(params, "tmsi") : NULL;
    if (tmsi_string) {
        uint64_t tmsi = xcn_string_uint64(tmsi_string);
        if (!tmsi)
            return xcn_send_error(stream, recvmsg,
                    OGS_SBI_HTTP_STATUS_BAD_REQUEST, "Invalid TMSI");

        tmsi_supi = xcn_supi_from_amf_tmsi(tmsi);
        if (!tmsi_supi)
            return xcn_send_error(stream, recvmsg,
                    OGS_SBI_HTTP_STATUS_NOT_FOUND, "No TMSI");

        pcf_ue_sm = pcf_ue_sm_find_by_supi(tmsi_supi);
        if (pcf_ue_sm) {
            pcf_xcn_refresh_ngap_ids_from_amf();
            root = xcn_user_to_json(pcf_ue_sm);
        } else {
            pcf_ue_am = pcf_ue_am_find_by_supi(tmsi_supi);
            if (!pcf_ue_am) {
                ogs_free(tmsi_supi);
                return xcn_send_error(stream, recvmsg,
                        OGS_SBI_HTTP_STATUS_NOT_FOUND, "No TMSI");
            }

            root = cJSON_CreateObject();
            ogs_assert(root);
            cJSON_AddStringToObject(root, "supi", pcf_ue_am->supi);
            cJSON_AddStringToObject(root, "imsi",
                    xcn_supi_to_imsi(pcf_ue_am->supi));
            cJSON_AddBoolToObject(root, "registered", true);
            users = cJSON_AddArrayToObject(root, "sessions");
            ogs_assert(users);
        }

        if (root)
            cJSON_AddNumberToObject(root, "m_tmsi", tmsi);
        ogs_free(tmsi_supi);

        if (xcn_send_json_response(
                    stream, OGS_SBI_HTTP_STATUS_OK, root) == false) {
            cJSON_Delete(root);
            return false;
        }

        cJSON_Delete(root);
        return true;
    }

    root = cJSON_CreateObject();
    ogs_assert(root);

    pcf_xcn_refresh_ngap_ids_from_amf();

    cJSON_AddNumberToObject(root, "registeredUserCount",
            ogs_list_count(&self->pcf_ue_am_list));
    cJSON_AddNumberToObject(root, "sessionUserCount",
            ogs_list_count(&self->pcf_ue_sm_list));
    users = cJSON_AddArrayToObject(root, "users");
    ogs_assert(users);

    ogs_list_for_each(&self->pcf_ue_sm_list, pcf_ue_sm) {
        cJSON_AddItemToArray(users, xcn_user_to_json(pcf_ue_sm));
        user_count++;
    }

    ogs_list_for_each(&self->pcf_ue_am_list, pcf_ue_am) {
        if (!pcf_ue_sm_find_by_supi(pcf_ue_am->supi)) {
            cJSON *user = cJSON_CreateObject();
            cJSON *sessions = NULL;
            ogs_assert(user);
            cJSON_AddStringToObject(user, "supi", pcf_ue_am->supi);
            cJSON_AddStringToObject(user, "imsi",
                    xcn_supi_to_imsi(pcf_ue_am->supi));
            cJSON_AddBoolToObject(user, "registered", true);
            sessions = cJSON_AddArrayToObject(user, "sessions");
            ogs_assert(sessions);
            cJSON_AddItemToArray(users, user);
            user_count++;
        }
    }
    cJSON_AddNumberToObject(root, "userCount", user_count);

    if (xcn_send_json_response(stream, OGS_SBI_HTTP_STATUS_OK, root) == false) {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

bool pcf_xcn_query_handle_user(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg)
{
    const char *supi = NULL;
    cJSON *root = NULL, *sessions = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;
    pcf_ue_am_t *pcf_ue_am = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    supi = recvmsg->h.resource.component[1];
    if (!supi)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No SUPI");

    pcf_ue_sm = pcf_ue_sm_find_by_supi((char *)supi);
    if (pcf_ue_sm) {
        pcf_xcn_refresh_ngap_ids_from_amf();
        root = xcn_user_to_json(pcf_ue_sm);
    } else {
        pcf_ue_am = pcf_ue_am_find_by_supi((char *)supi);
        if (!pcf_ue_am)
            return xcn_send_error(stream, recvmsg,
                    OGS_SBI_HTTP_STATUS_NOT_FOUND, "No SUPI");

        root = cJSON_CreateObject();
        ogs_assert(root);
        cJSON_AddStringToObject(root, "supi", pcf_ue_am->supi);
        cJSON_AddStringToObject(root, "imsi", xcn_supi_to_imsi(pcf_ue_am->supi));
        cJSON_AddBoolToObject(root, "registered", true);
        sessions = cJSON_AddArrayToObject(root, "sessions");
        ogs_assert(sessions);
    }

    if (xcn_send_json_response(stream, OGS_SBI_HTTP_STATUS_OK, root) == false) {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}

bool pcf_xcn_query_handle_sessions(
        ogs_sbi_stream_t *stream, ogs_sbi_message_t *recvmsg,
        const char *ue_ip)
{
    cJSON *root = NULL;
    pcf_sess_t *sess = NULL;
    pcf_ue_sm_t *pcf_ue_sm = NULL;

    ogs_assert(stream);
    ogs_assert(recvmsg);

    if (!ue_ip)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_BAD_REQUEST, "No ueIp");

    sess = pcf_sess_find_by_ipv4addr((char *)ue_ip);
    if (!sess)
        sess = pcf_sess_find_by_ipv6prefix((char *)ue_ip);
    if (sess && !sess->amf_ue_ngap_id && !sess->ran_ue_ngap_id) {
        pcf_xcn_refresh_ngap_ids_from_amf();
        sess = pcf_sess_find_by_ipv4addr((char *)ue_ip);
        if (!sess)
            sess = pcf_sess_find_by_ipv6prefix((char *)ue_ip);
    }

    if (!sess)
        return xcn_send_error(stream, recvmsg,
                OGS_SBI_HTTP_STATUS_NOT_FOUND, "No UE IP");

    pcf_ue_sm = pcf_ue_sm_find_by_id(sess->pcf_ue_sm_id);
    ogs_assert(pcf_ue_sm);

    root = cJSON_CreateObject();
    ogs_assert(root);
    cJSON_AddStringToObject(root, "supi", pcf_ue_sm->supi);
    cJSON_AddStringToObject(root, "imsi", xcn_supi_to_imsi(pcf_ue_sm->supi));
    cJSON_AddItemToObject(root, "session", xcn_session_to_json(sess));

    if (xcn_send_json_response(stream, OGS_SBI_HTTP_STATUS_OK, root) == false) {
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return true;
}
