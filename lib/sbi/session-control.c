/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "session-control.h"
#include <math.h>
#include <ctype.h>

bool ogs_sbi_session_control_id(const char *s, bool uuid)
{
    size_t i, n = s ? strlen(s) : 0;
    if (uuid && n != OGS_UUID_FORMATTED_LENGTH) return false;
    if (!uuid && (n < 16 || n > 80)) return false;
    for (i = 0; i < n; i++) {
        if (uuid && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (s[i] != '-') return false;
        } else if (uuid ? !isxdigit((unsigned char)s[i]) :
                !(isalnum((unsigned char)s[i]) || s[i] == '-')) return false;
    }
    return true;
}

bool ogs_sbi_session_ambr_encode(uint64_t bps, uint8_t *unit, uint16_t *value)
{
    static const uint64_t units[] = {
        1000, 4000, 16000, 64000, 256000, 1000000, 4000000,
        16000000, 64000000, 256000000, 1000000000,
        4000000000ULL, 16000000000ULL, 64000000000ULL,
        256000000000ULL, 1000000000000ULL
    };
    unsigned i;
    /* The NGAP BitRate range is the tightest of N2, NAS and PFCP. */
    if (!bps || bps > 4000000000000ULL) return false;
    for (i = 0; i < (sizeof(units) / sizeof(units[0])); i++) {
        if (bps % units[i] == 0 && bps / units[i] <= UINT16_MAX) {
            if (unit) *unit = i + 1;
            if (value) *value = bps / units[i];
            return true;
        }
    }
    return false;
}

bool ogs_sbi_session_bitrate_parse(const char *s, uint64_t *bps)
{
    uint64_t whole = 0, fraction = 0, scale = 1, multiplier;
    unsigned digits = 0;
    if (!s || strlen(s) > 40 || !isdigit((unsigned char)*s)) return false;
    while (isdigit((unsigned char)*s)) {
        if (++digits > 13) return false;
        whole = whole * 10 + (*s++ - '0');
    }
    if (*s == '.') {
        s++; digits = 0;
        while (isdigit((unsigned char)*s)) {
            if (++digits > 6) return false;
            fraction = fraction * 10 + (*s++ - '0'); scale *= 10;
        }
        if (!digits) return false;
    }
    if (*s++ != ' ') return false;
    if (!strcmp(s, "bps")) multiplier = 1;
    else if (!strcmp(s, "Kbps")) multiplier = 1000;
    else if (!strcmp(s, "Mbps")) multiplier = 1000000;
    else if (!strcmp(s, "Gbps")) multiplier = 1000000000;
    else if (!strcmp(s, "Tbps")) multiplier = 1000000000000ULL;
    else return false;
    if (whole > 4000000000000ULL / multiplier ||
        (fraction * multiplier) % scale) return false;
    *bps = whole * multiplier + fraction * multiplier / scale;
    return ogs_sbi_session_ambr_encode(*bps, NULL, NULL);
}

static bool keys(const cJSON *object, const char *const *allowed)
{
    const cJSON *item, *other;
    unsigned i;
    if (!cJSON_IsObject(object)) return false;
    cJSON_ArrayForEach(item, object) {
        for (i = 0; allowed[i] && strcmp(item->string, allowed[i]); i++);
        if (!allowed[i]) return false;
        for (other = item->next; other; other = other->next)
            if (!strcmp(item->string, other->string)) return false;
    }
    return true;
}

static bool number(const cJSON *object, const char *key, int min, int max, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max ||
        item->valuedouble != item->valueint) return false;
    *value = item->valueint;
    return true;
}

bool ogs_sbi_session_config_parse(const cJSON *body, ogs_sbi_session_config_t *out)
{
    static const char *const root_keys[] = {"defaultQos", "sessionAmbr", NULL};
    static const char *const qos_keys[] = {"5qi", "priorityLevel", "arp", NULL};
    static const char *const arp_keys[] = {"priorityLevel", "preemptionCapability", "preemptionVulnerability", NULL};
    static const char *const ambr_keys[] = {"uplink", "downlink", NULL};
    const cJSON *q = cJSON_GetObjectItemCaseSensitive(body, "defaultQos");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(q, "arp");
    const cJSON *r = cJSON_GetObjectItemCaseSensitive(body, "sessionAmbr");
    const char *cap = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "preemptionCapability"));
    const char *vuln = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "preemptionVulnerability"));
    ogs_sbi_session_config_t c = {0};
    int v;
    if (!keys(body, root_keys) || !keys(q, qos_keys) ||
        !keys(a, arp_keys) || !keys(r, ambr_keys) ||
        !number(q, "5qi", 1, 255, &v)) return false;
    switch (v) {
    case 5: case 6: case 7: case 8: case 9: case 10:
    case 69: case 70: case 79: case 80: break;
    default: return false;
    }
    c.qos.index = v;
    if (cJSON_HasObjectItem(q, "priorityLevel")) {
        if (!number(q, "priorityLevel", 1, 127, &v)) return false;
        c.qos.priority_level = v;
    }
    if (!number(a, "priorityLevel", 1, 15, &v) || !cap || !vuln) return false;
    c.qos.arp.priority_level = v;
    if (!strcmp(cap, "MAY_PREEMPT")) c.qos.arp.pre_emption_capability = OGS_5GC_PRE_EMPTION_ENABLED;
    else if (!strcmp(cap, "NOT_PREEMPT")) c.qos.arp.pre_emption_capability = OGS_5GC_PRE_EMPTION_DISABLED;
    else return false;
    if (!strcmp(vuln, "PREEMPTABLE")) c.qos.arp.pre_emption_vulnerability = OGS_5GC_PRE_EMPTION_ENABLED;
    else if (!strcmp(vuln, "NOT_PREEMPTABLE")) c.qos.arp.pre_emption_vulnerability = OGS_5GC_PRE_EMPTION_DISABLED;
    else return false;
    if (!ogs_sbi_session_bitrate_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "uplink")), &c.ambr.uplink) ||
        !ogs_sbi_session_bitrate_parse(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "downlink")), &c.ambr.downlink)) return false;
    *out = c;
    return true;
}

cJSON *ogs_sbi_session_config_json(const ogs_sbi_session_config_t *c)
{
    cJSON *body = cJSON_CreateObject(), *q, *a, *r;
    char text[40];
    q = cJSON_AddObjectToObject(body, "defaultQos");
    cJSON_AddNumberToObject(q, "5qi", c->qos.index);
    if (c->qos.priority_level) cJSON_AddNumberToObject(q, "priorityLevel", c->qos.priority_level);
    a = cJSON_AddObjectToObject(q, "arp");
    cJSON_AddNumberToObject(a, "priorityLevel", c->qos.arp.priority_level);
    cJSON_AddStringToObject(a, "preemptionCapability", c->qos.arp.pre_emption_capability == OGS_5GC_PRE_EMPTION_ENABLED ? "MAY_PREEMPT" : "NOT_PREEMPT");
    cJSON_AddStringToObject(a, "preemptionVulnerability", c->qos.arp.pre_emption_vulnerability == OGS_5GC_PRE_EMPTION_ENABLED ? "PREEMPTABLE" : "NOT_PREEMPTABLE");
    r = cJSON_AddObjectToObject(body, "sessionAmbr");
    ogs_snprintf(text, sizeof(text), "%llu bps", (unsigned long long)c->ambr.uplink);
    cJSON_AddStringToObject(r, "uplink", text);
    ogs_snprintf(text, sizeof(text), "%llu bps", (unsigned long long)c->ambr.downlink);
    cJSON_AddStringToObject(r, "downlink", text);
    return body;
}

bool ogs_sbi_session_qos_equal(const ogs_qos_t *a, const ogs_qos_t *b)
{
    return a->index == b->index && a->priority_level == b->priority_level &&
        a->arp.priority_level == b->arp.priority_level &&
        a->arp.pre_emption_capability == b->arp.pre_emption_capability &&
        a->arp.pre_emption_vulnerability == b->arp.pre_emption_vulnerability;
}

bool ogs_sbi_session_config_equal(const ogs_sbi_session_config_t *a, const ogs_sbi_session_config_t *b)
{
    return ogs_sbi_session_qos_equal(&a->qos, &b->qos) &&
        a->ambr.uplink == b->ambr.uplink && a->ambr.downlink == b->ambr.downlink;
}

const char *ogs_sbi_session_control_state(int state)
{
    static const char *const names[] = {"not-required", "pending", "confirmed", "failed", "unknown"};
    return state >= 0 && state < (sizeof(names) / sizeof(names[0])) ? names[state] : "unknown";
}

cJSON *ogs_sbi_session_operation_json(const ogs_sbi_session_operation_t *op)
{
    cJSON *body = cJSON_CreateObject(), *stages = cJSON_AddObjectToObject(body, "stages");
    cJSON_AddStringToObject(body, "instance", op->instance);
    cJSON_AddStringToObject(body, "transaction", op->transaction);
    cJSON_AddStringToObject(body, "operationId", op->operation_id);
    cJSON_AddStringToObject(body, "state", ogs_sbi_session_control_state(op->state));
    cJSON_AddStringToObject(body, "detail", op->detail);
    cJSON_AddNumberToObject(body, "updatedAtUs", op->updated_at);
    cJSON_AddBoolToObject(body, "rateEnforcementVerified", false);
    cJSON_AddStringToObject(stages, "pfcp", ogs_sbi_session_control_state(op->pfcp));
    cJSON_AddStringToObject(stages, "nas", ogs_sbi_session_control_state(op->nas));
    cJSON_AddStringToObject(stages, "ngap", ogs_sbi_session_control_state(op->ngap));
    cJSON_AddItemToObject(body, "previous", ogs_sbi_session_config_json(&op->previous));
    cJSON_AddItemToObject(body, "target", ogs_sbi_session_config_json(&op->target));
    return body;
}

static int state_parse(const cJSON *body, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, key));
    int i;
    if (s) for (i = 0; i <= OGS_SESSION_CONTROL_UNKNOWN; i++)
        if (!strcmp(s, ogs_sbi_session_control_state(i))) return i;
    return -1;
}

bool ogs_sbi_session_operation_result(const cJSON *body, ogs_sbi_session_operation_t *op)
{
    const cJSON *stages = cJSON_GetObjectItemCaseSensitive(body, "stages");
    const char *detail = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(body, "detail"));
    int state = state_parse(body, "state"), p = state_parse(stages, "pfcp"),
        n = state_parse(stages, "nas"), g = state_parse(stages, "ngap");
    if (state < OGS_SESSION_CONTROL_CONFIRMED || p < 0 || n < 0 || g < 0 ||
        !detail || strlen(detail) >= sizeof(op->detail)) return false;
    if (state == OGS_SESSION_CONTROL_CONFIRMED &&
        (p != OGS_SESSION_CONTROL_CONFIRMED ||
         (n != OGS_SESSION_CONTROL_NONE && n != OGS_SESSION_CONTROL_CONFIRMED) ||
         g != OGS_SESSION_CONTROL_CONFIRMED)) return false;
    op->state = state; op->pfcp = p; op->nas = n; op->ngap = g;
    ogs_cpystrn(op->detail, detail, sizeof(op->detail));
    op->updated_at = ogs_time_now();
    return true;
}
