/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <curl/curl.h>
#include <ctype.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "inspect.h"

#define PAGE_LIMIT 1024
#define PAGE_BYTES (4 * 1024 * 1024)
#define TOTAL_BYTES (16 * 1024 * 1024)
#define RATE_BYTES (8 * 1024 * 1024)

static const cJSON *field(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static const char *string(const cJSON *obj, const char *key)
{
    const cJSON *v = field(obj, key);
    return cJSON_IsString(v) && v->valuestring[0] ? v->valuestring : "-";
}

static bool number(const cJSON *v, uint64_t maximum, uint64_t *result)
{
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < 0 || v->valuedouble > (double)maximum)
        return false;
    *result = (uint64_t)v->valuedouble;
    return !((double)*result < v->valuedouble || (double)*result > v->valuedouble);
}

static bool set(cJSON *obj, const char *key, cJSON *value)
{
    if (!value) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(obj, key);
    if (cJSON_AddItemToObject(obj, key, value)) return true;
    cJSON_Delete(value);
    return false;
}

typedef struct {
    char *data;
    size_t size;
} response_t;

static size_t receive(void *data, size_t size, size_t count, void *opaque)
{
    response_t *r = opaque;
    size_t bytes;
    char *expanded;

    if (size && count > SIZE_MAX / size) return 0;
    bytes = size * count;
    if (bytes > PAGE_BYTES - r->size) return 0;
    expanded = realloc(r->data, r->size + bytes + 1);
    if (!expanded) return 0;
    r->data = expanded;
    memcpy(r->data + r->size, data, bytes);
    r->size += bytes;
    r->data[r->size] = '\0';
    return bytes;
}

cJSON *xcnctl_load_info(const char *base)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *all = root ? cJSON_AddArrayToObject(root, "items") : NULL;
    size_t total = 0;
    unsigned int page;

    if (!all || !base || (strncmp(base, "http://", 7) &&
                strncmp(base, "https://", 8))) goto fail;
    for (page = 0; page < PAGE_LIMIT; page++) {
        CURL *curl;
        CURLcode result;
        response_t response = {0};
        cJSON *doc, *items, *pager, *next, *item;
        uint64_t value;
        char url[2048];
        long status = 0;
        int len = snprintf(url, sizeof(url), "%s%cpage=%u&page_size=100",
                base, strchr(base, '?') ? '&' : '?', page);

        if (len < 0 || (size_t)len >= sizeof(url)) goto fail;
        curl = curl_easy_init();
        if (!curl) goto fail;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 200L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 1000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        result = curl_easy_perform(curl);
        if (result == CURLE_OK)
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_cleanup(curl);
        if (result != CURLE_OK || status != 200 || !response.data ||
            response.size > TOTAL_BYTES - total) {
            free(response.data);
            goto fail;
        }
        total += response.size;
        if (memchr(response.data, '\0', response.size)) {
            free(response.data);
            goto fail;
        }
        doc = cJSON_ParseWithOpts(response.data, NULL, true);
        free(response.data);
        items = cJSON_GetObjectItemCaseSensitive(doc, "items");
        pager = cJSON_GetObjectItemCaseSensitive(doc, "pager");
        next = cJSON_GetObjectItemCaseSensitive(pager, "next");
        if (!cJSON_IsArray(items) || !cJSON_IsObject(pager) ||
            !number(field(pager, "page"), UINT32_MAX, &value) || value != page ||
            !number(field(pager, "count"), 100, &value) ||
            value != (uint64_t)cJSON_GetArraySize(items) ||
            (field(pager, "truncated") &&
             !cJSON_IsFalse(field(pager, "truncated"))) ||
            (next && !cJSON_IsNull(next) &&
             (!cJSON_IsString(next) || !next->valuestring[0] || !value))) {
            cJSON_Delete(doc);
            goto fail;
        }
        bool more = cJSON_IsString(next);
        while ((item = cJSON_DetachItemFromArray(items, 0))) {
            if (!cJSON_IsObject(item) || !cJSON_AddItemToArray(all, item)) {
                cJSON_Delete(item);
                cJSON_Delete(doc);
                goto fail;
            }
        }
        cJSON_Delete(doc);
        if (!more) return root;
    }
fail:
    cJSON_Delete(root);
    return NULL;
}

static bool gnb_identity(const cJSON *gnb, uint64_t *id, uint64_t *bits)
{
    return number(field(gnb, "gnb_id_bits"), 32, bits) && *bits >= 22 &&
        number(field(gnb, "gnb_id"), UINT32_MAX, id) && *id < (1ULL << *bits);
}

static cJSON *location(const cJSON *ue)
{
    const cJSON *source = field(ue, "location");
    cJSON *loc = cJSON_IsObject(source) ?
        cJSON_Duplicate(source, true) : cJSON_CreateObject();
    cJSON *cgi;
    uint64_t timestamp = 0, nci = 0, id = 0, bits = 0;
    bool known, valid_bits, valid_nci, decoded, matches;
    char hex[12];

    if (!loc) return NULL;
    if (!number(field(loc, "timestamp"), 9007199254740991ULL, &timestamp) ||
        !timestamp) {
        if (!set(loc, "timestamp", cJSON_CreateNull()) ||
            !set(loc, "status", cJSON_CreateString("unknown")) ||
            !set(loc, "gnb", cJSON_CreateNull()) ||
            !set(loc, "nr_cgi", cJSON_CreateNull()) ||
            !set(loc, "nr_tai", cJSON_CreateNull())) goto fail;
        return loc;
    }
    if (!set(loc, "status", cJSON_CreateString(
                !strcmp(string(ue, "cm_state"), "connected") ? "reported" :
                !strcmp(string(ue, "cm_state"), "idle") ? "last-known" :
                "unknown"))) goto fail;
    cgi = cJSON_GetObjectItemCaseSensitive(loc, "nr_cgi");
    const cJSON *length = field(field(loc, "gnb"), "gnb_id_bits");
    if (!length) length = field(cgi, "gnb_id_bits");
    valid_bits = number(length, 32, &bits) && bits >= 22;
    uint64_t reported_bits = 0;
    known = gnb_identity(field(loc, "gnb"), &id, &reported_bits);
    if (!known && !set(loc, "gnb", cJSON_CreateNull())) goto fail;
    if (!cJSON_IsObject(cgi)) return loc;
    valid_nci = number(field(cgi, "nci"), 0xFFFFFFFFFULL, &nci);
    decoded = valid_bits && valid_nci;
    matches = decoded && known && (nci >> (36 - bits)) == id;
    if (valid_nci) {
        snprintf(hex, sizeof(hex), "0x%09" PRIx64, nci);
        if (!set(cgi, "nci_hex", cJSON_CreateString(hex))) goto fail;
    } else if (!set(cgi, "nci", cJSON_CreateNull()) ||
               !set(cgi, "nci_hex", cJSON_CreateNull())) goto fail;
    if (!set(cgi, "gnb_id_bits", valid_bits ? cJSON_CreateNumber(bits) : cJSON_CreateNull()) ||
        !set(cgi, "identity_consistent", known && decoded ?
            cJSON_CreateBool(matches) : cJSON_CreateNull()) ||
        !set(cgi, "gnb_id", decoded ? cJSON_CreateNumber(nci >> (36 - bits)) : cJSON_CreateNull()) ||
        !set(cgi, "cell_id", decoded ?
            cJSON_CreateNumber(nci & ((1ULL << (36 - bits)) - 1)) : cJSON_CreateNull()))
        goto fail;
    return loc;
fail:
    cJSON_Delete(loc);
    return NULL;
}

static bool prepare_ue(cJSON *ue)
{
    cJSON *gnb = cJSON_GetObjectItemCaseSensitive(ue, "gnb");
    const cJSON *loc = field(ue, "location");
    const cJSON *sessions = field(ue, "pdu_sessions"), *session;
    uint64_t id, bits;

    if ((gnb && !cJSON_IsObject(gnb) && !cJSON_IsNull(gnb)) ||
        (loc && !cJSON_IsObject(loc) && !cJSON_IsNull(loc)) ||
        (sessions && !cJSON_IsArray(sessions))) return false;
    cJSON_ArrayForEach(session, sessions)
        if (!cJSON_IsObject(session)) return false;
    /* Old AMFs derive this value with a fixed 22-bit split: do not trust it. */
    if (cJSON_IsObject(gnb) && !gnb_identity(gnb, &id, &bits)) {
        if (!set(gnb, "gnb_id", cJSON_CreateNull()) ||
            !set(gnb, "gnb_id_bits", cJSON_CreateNull()) ||
            !set(gnb, "cell_id", cJSON_CreateNull())) return false;
    }
    return set(ue, "location", location(ue));
}

static const cJSON *ue_gnb(const cJSON *ue)
{
    uint64_t id, bits;
    const cJSON *gnb = field(ue, "gnb");
    return gnb_identity(gnb, &id, &bits) ? gnb : field(field(ue, "location"), "gnb");
}

static const cJSON *find_ue(const cJSON *root, const char *supi)
{
    const cJSON *ue, *found = NULL;
    if (!supi || !*supi || !strcmp(supi, "-")) return NULL;
    cJSON_ArrayForEach(ue, field(root, "items")) {
        if (!strcmp(string(ue, "supi"), supi)) {
            if (found) return NULL;
            found = ue;
        }
    }
    return found;
}

static bool session_compatible(const cJSON *amf, const cJSON *smf)
{
    const cJSON *a = field(amf, "snssai"), *b = field(smf, "snssai");
    const cJSON *ad = field(amf, "dnn"), *bd = field(smf, "dnn");
    uint64_t ast, bst;

    if (cJSON_IsString(ad) && cJSON_IsString(bd) &&
        strcmp(ad->valuestring, bd->valuestring)) return false;
    if (number(field(a, "sst"), 255, &ast) &&
        number(field(b, "sst"), 255, &bst) && ast != bst) return false;
    ad = field(a, "sd");
    bd = field(b, "sd");
    return !cJSON_IsString(ad) || !cJSON_IsString(bd) ||
        !strcasecmp(ad->valuestring, bd->valuestring);
}

static bool enrich_sessions(cJSON *ue, const cJSON *smf)
{
    const cJSON *smf_ue = find_ue(smf, string(ue, "supi"));
    cJSON *session;

    cJSON_ArrayForEach(session, cJSON_GetObjectItemCaseSensitive(ue, "pdu_sessions")) {
        const cJSON *pdu, *found = NULL;
        uint64_t psi, candidate;
        bool ambiguous = false;
        if (number(field(session, "psi"), 255, &psi) && psi &&
            cJSON_IsArray(field(smf_ue, "pdu"))) {
            cJSON_ArrayForEach(pdu, field(smf_ue, "pdu")) {
                if (number(field(pdu, "psi"), 255, &candidate) && candidate == psi) {
                    if (found) { ambiguous = true; break; }
                    found = pdu;
                }
            }
        }
        if (ambiguous || (found && !session_compatible(session, found))) found = NULL;
        const char *keys[] = {"ipv4", "ipv6", "pdu_state", "upf_seid"};
        size_t i;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const cJSON *v = field(found, keys[i]);
            if (!set(session, keys[i], cJSON_IsString(v) ?
                        cJSON_Duplicate(v, true) : cJSON_CreateNull())) return false;
        }
        const cJSON *qos = field(found, "qos_flows"), *n3 = field(found, "n3");
        if (!set(session, "qos_flows", cJSON_IsArray(qos) ?
                    cJSON_Duplicate(qos, true) : cJSON_CreateNull()) ||
            !set(session, "n3", cJSON_IsObject(n3) ?
                    cJSON_Duplicate(n3, true) : cJSON_CreateNull()) ||
            !set(session, "smf_info_status", cJSON_CreateString(
                    !smf ? "unavailable" : found ? "available" : "not-matched")))
            return false;
    }
    return true;
}

static bool same_gnb(const cJSON *a, const cJSON *b)
{
    uint64_t aid, abits, bid, bbits;
    return gnb_identity(a, &aid, &abits) && gnb_identity(b, &bid, &bbits) &&
        aid == bid && abits == bbits && strcmp(string(a, "plmn"), "-") &&
        !strcmp(string(a, "plmn"), string(b, "plmn"));
}

static bool observe_cell(cJSON *cells, const cJSON *ue)
{
    const cJSON *loc = field(ue, "location"), *cgi = field(loc, "nr_cgi");
    cJSON *cell;
    uint64_t nci, candidate, latest = 0, timestamp = 0, count = 0;
    const char *keys[] = {"gnb", "nr_cgi", "nr_tai", "timestamp", "status"};
    const char *state = string(ue, "cm_state");
    const char *counter = !strcmp(state, "connected") ? "connected_ue_count" :
        !strcmp(state, "idle") ? "idle_ue_count" : "unknown_ue_count";
    size_t i;

    if (!number(field(cgi, "nci"), 0xFFFFFFFFFULL, &nci) ||
        !number(field(loc, "timestamp"), 9007199254740991ULL, &timestamp) ||
        !timestamp || !strcmp(string(cgi, "plmn"), "-")) return true;
    cJSON_ArrayForEach(cell, cells) {
        const cJSON *other = field(cell, "nr_cgi");
        if (number(field(other, "nci"), 0xFFFFFFFFFULL, &candidate) && candidate == nci &&
            !strcmp(string(other, "plmn"), string(cgi, "plmn"))) break;
    }
    if (!cell) {
        cell = cJSON_CreateObject();
        if (!cell) return false;
        if (!cJSON_AddItemToArray(cells, cell)) { cJSON_Delete(cell); return false; }
        if (!set(cell, "ue_count", cJSON_CreateNumber(0)) ||
            !set(cell, "connected_ue_count", cJSON_CreateNumber(0)) ||
            !set(cell, "idle_ue_count", cJSON_CreateNumber(0)) ||
            !set(cell, "unknown_ue_count", cJSON_CreateNumber(0))) return false;
    }
    (void)number(field(cell, "timestamp"), 9007199254740991ULL, &latest);
    if (timestamp >= latest) {
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const cJSON *v = field(loc, keys[i]);
            if (!set(cell, keys[i], v ? cJSON_Duplicate(v, true) : cJSON_CreateNull()))
                return false;
        }
    }
    (void)number(field(cell, "ue_count"), UINT32_MAX, &count);
    if (!set(cell, "ue_count", cJSON_CreateNumber(count + 1))) return false;
    count = 0;
    (void)number(field(cell, counter), UINT32_MAX, &count);
    return set(cell, counter, cJSON_CreateNumber(count + 1));
}

static bool enrich_gnb(cJSON *gnb, const cJSON *ues)
{
    cJSON *associated, *cells;
    const cJSON *ue;
    const char *peer = string(field(field(gnb, "ng"), "sctp"), "peer");

    if (!set(gnb, "ue_info_available", cJSON_CreateBool(ues != NULL)) ||
        !set(gnb, "associated_ues", ues ? cJSON_CreateArray() : cJSON_CreateNull()) ||
        !set(gnb, "observed_cells", ues ? cJSON_CreateArray() : cJSON_CreateNull()) ||
        !set(gnb, "observed_cells_source", cJSON_CreateString("ue-location-reports")))
        return false;
    associated = cJSON_GetObjectItemCaseSensitive(gnb, "associated_ues");
    cells = cJSON_GetObjectItemCaseSensitive(gnb, "observed_cells");
    cJSON_ArrayForEach(ue, field(ues, "items")) {
        const cJSON *current = field(ue, "gnb");
        const char *current_peer = string(current, "peer");
        if (!strcmp(string(ue, "cm_state"), "connected") && same_gnb(gnb, current) &&
            (!strcmp(peer, "-") || !strcmp(current_peer, "-") || !strcmp(peer, current_peer)) &&
            (!field(current, "status") || !strcmp(string(current, "status"), "associated"))) {
            cJSON *summary = cJSON_CreateObject();
            const char *keys[] = {"supi", "cm_state", "gnb"};
            const cJSON *sessions = field(ue, "pdu_sessions");
            size_t i;
            if (!summary) return false;
            if (!cJSON_AddItemToArray(associated, summary)) { cJSON_Delete(summary); return false; }
            for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                const cJSON *v = field(ue, keys[i]);
                if (!set(summary, keys[i], v ? cJSON_Duplicate(v, true) : cJSON_CreateNull()))
                    return false;
            }
            if (!set(summary, "pdu_sessions_count", cJSON_IsArray(sessions) ?
                        cJSON_CreateNumber(cJSON_GetArraySize(sessions)) : cJSON_CreateNull()))
                return false;
        }
        if (same_gnb(gnb, field(field(ue, "location"), "gnb")) && !observe_cell(cells, ue))
            return false;
    }
    return true;
}

static const char *decimal(const cJSON *obj, const char *key, char *buf, size_t size)
{
    uint64_t value;
    if (!number(field(obj, key), 9007199254740991ULL, &value)) return "-";
    snprintf(buf, size, "%" PRIu64, value);
    return buf;
}

static const char *location_label(const cJSON *loc)
{
    return cJSON_IsFalse(field(field(loc, "nr_cgi"), "identity_consistent")) ?
        "identity-mismatch" : string(loc, "status");
}

static void detail_field(int indent, const char *label, const char *value)
{
    printf("%*s%-24s: %s\n", indent, "", label, value);
}

static void detail_number(int indent, const char *label, const cJSON *obj, const char *key)
{
    char value[24];
    detail_field(indent, label, decimal(obj, key, value, sizeof(value)));
}

static const char *yes_no(const cJSON *value)
{
    uint64_t n;
    if (cJSON_IsBool(value)) return cJSON_IsTrue(value) ? "yes" : "no";
    return number(value, 1, &n) ? (n ? "yes" : "no") : "-";
}

static void detail_strings(int indent, const char *label, const cJSON *values)
{
    const cJSON *value;
    bool first = true;
    printf("%*s%-24s: ", indent, "", label);
    if (!cJSON_IsArray(values)) { puts("-"); return; }
    cJSON_ArrayForEach(value, values) {
        if (!first) fputs(", ", stdout);
        fputs(cJSON_IsString(value) ? value->valuestring : "-", stdout);
        first = false;
    }
    puts(first ? "(none)" : "");
}

static void detail_slices(int indent, const cJSON *slices)
{
    const cJSON *slice;
    char value[24];
    if (!cJSON_IsArray(slices) || !cJSON_GetArraySize(slices)) {
        printf("%*s%s\n", indent, "", cJSON_IsArray(slices) ? "(none)" : "-");
        return;
    }
    cJSON_ArrayForEach(slice, slices)
        printf("%*sSST %-3s SD %s\n", indent, "",
                decimal(slice, "sst", value, sizeof(value)), string(slice, "sd"));
}

static void detail_location(int indent, const cJSON *loc)
{
    const cJSON *cgi = field(loc, "nr_cgi"), *gnb = field(loc, "gnb");
    const cJSON *tai = field(loc, "nr_tai"), *consistent = field(cgi, "identity_consistent");
    uint64_t timestamp;
    char value[24];

    printf("%*s%-24s: %s-%s\n", indent, "", "NR CGI", string(cgi, "plmn"), string(cgi, "nci_hex"));
    printf("%*s%-24s: %s (%s)\n", indent, "", "NCI",
            decimal(cgi, "nci", value, sizeof(value)), string(cgi, "nci_hex"));
    detail_number(indent, "gNB ID (reported)", gnb, "gnb_id");
    detail_number(indent, "gNB ID (from NCI)", cgi, "gnb_id");
    detail_number(indent, "gNB ID length (bits)", cgi, "gnb_id_bits");
    detail_number(indent, "Cell local ID", cgi, "cell_id");
    detail_field(indent, "gNB PLMN", string(gnb, "plmn"));
    detail_field(indent, "gNB peer", string(gnb, "peer"));
    printf("%*s%-24s: PLMN %s  TAC %s (0x%s)\n", indent, "", "TAI",
            string(tai, "plmn"), decimal(tai, "tac", value, sizeof(value)), string(tai, "tac_hex"));
    detail_field(indent, "Location", string(loc, "status"));
    detail_field(indent, "Identity", cJSON_IsTrue(consistent) ? "consistent" :
            cJSON_IsFalse(consistent) ? "identity-mismatch" : "unknown");
    if (number(field(loc, "timestamp"), 9007199254740991ULL, &timestamp) && timestamp) {
        time_t seconds = timestamp / 1000000;
        struct tm utc;
        char date[32];
        if (gmtime_r(&seconds, &utc) && strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc)) {
            double age = difftime(time(NULL), seconds);
            printf("%*s%-24s: %s (age %.0f s)\n", indent, "", "Location updated", date, age > 0 ? age : 0);
            return;
        }
    }
    detail_field(indent, "Location updated", "-");
}

static void detail_endpoint(int indent, const char *label, const cJSON *endpoint)
{
    uint64_t teid;
    printf("%*s%s\n", indent, "", label);
    detail_field(indent + 2, "Address", string(endpoint, "addr"));
    if (number(field(endpoint, "teid"), UINT32_MAX, &teid))
        printf("%*s%-24s: %" PRIu64 " (0x%08" PRIx64 ")\n", indent + 2, "", "TEID", teid, teid);
    else detail_field(indent + 2, "TEID", "-");
    if (field(endpoint, "pdr_id")) detail_number(indent + 2, "PDR-ID", endpoint, "pdr_id");
}

static void print_ue_detail(const cJSON *ue)
{
    const cJSON *ran = field(ue, "gnb"), *security = field(ue, "security"), *sessions = field(ue, "pdu_sessions");
    const cJSON *session, *ambr = field(ue, "ambr");
    uint64_t rate;
    char value[24];

    printf("UE %s\n  Identity\n", string(ue, "supi"));
    detail_field(4, "CM-STATE", string(ue, "cm_state"));
    detail_field(4, "SUCI", string(ue, "suci"));
    detail_field(4, "PEI", string(ue, "pei"));
    detail_field(4, "GUTI", string(ue, "guti"));
    detail_strings(4, "MSISDN", field(ue, "msisdn"));
    puts("  Current RAN association");
    detail_field(4, "Status", string(ran, "status"));
    detail_number(4, "GNB-ID", ran, "gnb_id");
    detail_number(4, "GNB-LEN (bits)", ran, "gnb_id_bits");
    detail_field(4, "PLMN", string(ran, "plmn"));
    detail_field(4, "SCTP peer", string(ran, "peer"));
    detail_number(4, "AMF-UE-NGAP-ID", ran, "amf_ue_ngap_id");
    detail_number(4, "RAN-UE-NGAP-ID", ran, "ran_ue_ngap_id");
    detail_number(4, "SCTP stream ID", ran, "ostream_id");
    puts("  Location (last NGAP report)");
    detail_location(4, field(ue, "location"));
    puts("  Security and subscription");
    detail_field(4, "Security context valid", yes_no(field(security, "valid")));
    detail_field(4, "NAS encryption", string(security, "enc"));
    detail_field(4, "NAS integrity", string(security, "int"));
    if (number(field(ambr, "uplink"), 9007199254740991ULL, &rate))
        printf("    %-24s: %.3f Mbps (%" PRIu64 " bps)\n", "UE-AMBR UL", (double)rate / 1000000.0, rate);
    else detail_field(4, "UE-AMBR UL", "-");
    if (number(field(ambr, "downlink"), 9007199254740991ULL, &rate))
        printf("    %-24s: %.3f Mbps (%" PRIu64 " bps)\n", "UE-AMBR DL", (double)rate / 1000000.0, rate);
    else detail_field(4, "UE-AMBR DL", "-");
    detail_strings(4, "AM policy features", field(field(ue, "am_policy_features_info"), "labels"));
    puts("    Requested slices");
    detail_slices(6, field(ue, "requested_slices"));
    puts("    Allowed slices");
    detail_slices(6, field(ue, "allowed_slices"));
    puts("  PDU Sessions");
    if (!cJSON_IsArray(sessions) || !cJSON_GetArraySize(sessions))
        printf("    %s\n", cJSON_IsArray(sessions) ? "(none)" : "-");
    cJSON_ArrayForEach(session, sessions) {
        const cJSON *slice = field(session, "snssai"), *qos = field(session, "qos_flows"), *flow;
        printf("    PSI %s\n", decimal(session, "psi", value, sizeof(value)));
        detail_field(6, "DNN", string(session, "dnn"));
        printf("      %-24s: SST %s  SD %s\n", "Slice", decimal(slice, "sst", value, sizeof(value)), string(slice, "sd"));
        detail_field(6, "PDU-STATE", string(session, "pdu_state"));
        detail_field(6, "SMF data", string(session, "smf_info_status"));
        detail_field(6, "IPv4", string(session, "ipv4"));
        detail_field(6, "IPv6", string(session, "ipv6"));
        detail_field(6, "UPF-SEID", string(session, "upf_seid"));
        detail_field(6, "N1 released", yes_no(field(session, "n1_released")));
        detail_field(6, "N2 released", yes_no(field(session, "n2_released")));
        puts("      QoS flows");
        if (!cJSON_IsArray(qos) || !cJSON_GetArraySize(qos))
            printf("        %s\n", cJSON_IsArray(qos) ? "(none)" : "-");
        cJSON_ArrayForEach(flow, qos) {
            char fiveqi[24];
            printf("        QFI %-3s 5QI %s\n", decimal(flow, "qfi", value, sizeof(value)),
                    decimal(flow, "5qi", fiveqi, sizeof(fiveqi)));
        }
        detail_endpoint(6, "N3 UL / UPF", field(field(session, "n3"), "upf"));
        detail_endpoint(6, "N3 DL / gNB", field(field(session, "n3"), "gnb"));
    }
    puts("");
}

static void print_gnb_detail(const cJSON *gnb)
{
    const cJSON *ng = field(gnb, "ng");
    const cJSON *sctp = field(ng, "sctp");
    const cJSON *setup = field(ng, "setup_success");
    const cJSON *tas = field(gnb, "supported_ta_list");
    const cJSON *ta, *ue, *cell;
    const cJSON *associated = field(gnb, "associated_ues"), *cells = field(gnb, "observed_cells");
    const char *state = cJSON_IsTrue(setup) ? "ready" :
        cJSON_IsFalse(setup) ? "pending" : "unknown";
    char value[24];

    printf("gNB %s\n", decimal(gnb, "gnb_id", value, sizeof(value)));
    printf("  %-18s: %s\n", "PLMN", string(gnb, "plmn"));
    printf("  %-18s: %s\n", "GNB-LEN (bits)",
            decimal(gnb, "gnb_id_bits", value, sizeof(value)));
    printf("  %-18s: %s\n", "NG-STATE", state);
    printf("  %-18s: %s\n", "UE-COUNT",
            decimal(gnb, "num_connected_ues", value, sizeof(value)));
    if (cJSON_IsArray(tas))
        snprintf(value, sizeof(value), "%d", cJSON_GetArraySize(tas));
    else
        snprintf(value, sizeof(value), "-");
    printf("  %-18s: %s\n", "TA-COUNT", value);
    puts("  NG/SCTP");
    printf("    %-16s: %s\n", "AMF-NAME", string(field(gnb, "network"), "amf_name"));
    printf("    %-16s: %s\n", "AMF-NGAP-PORT",
            decimal(field(gnb, "network"), "ngap_port", value, sizeof(value)));
    printf("    %-16s: %s\n", "PEER", string(sctp, "peer"));
    printf("    %-16s: %s\n", "MAX-OUT-STREAMS",
            decimal(sctp, "max_out_streams", value, sizeof(value)));
    printf("    %-16s: %s\n", "NEXT-STREAM-ID",
            decimal(sctp, "next_ostream_id", value, sizeof(value)));
    puts("  Supported TA/PLMN/slices");
    if (!cJSON_IsArray(tas) || !cJSON_GetArraySize(tas))
        printf("    %s\n", cJSON_IsArray(tas) ? "(none)" : "-");
    if (cJSON_IsArray(tas)) cJSON_ArrayForEach(ta, tas) {
        const cJSON *plmns = field(ta, "bplmns"), *plmn;
        printf("    TAC %s\n", string(ta, "tac"));
        if (!cJSON_IsArray(plmns) || !cJSON_GetArraySize(plmns)) {
            printf("      PLMNs: %s\n", cJSON_IsArray(plmns) ? "(none)" : "-");
            continue;
        }
        cJSON_ArrayForEach(plmn, plmns) {
            const cJSON *slices = field(plmn, "snssai"), *slice;
            printf("      PLMN %s\n", string(plmn, "plmn"));
            if (!cJSON_IsArray(slices) || !cJSON_GetArraySize(slices)) {
                printf("        Slices: %s\n", cJSON_IsArray(slices) ? "(none)" : "-");
                continue;
            }
            cJSON_ArrayForEach(slice, slices)
                printf("        SST %-3s SD %s\n",
                        decimal(slice, "sst", value, sizeof(value)), string(slice, "sd"));
        }
    }
    puts("  Associated UEs (current AMF contexts)");
    if (!cJSON_IsArray(associated) || !cJSON_GetArraySize(associated))
        printf("    %s\n", cJSON_IsArray(associated) ? "(none)" : "- (UE info unavailable)");
    cJSON_ArrayForEach(ue, associated) {
        printf("    UE %s  CM-STATE %s\n", string(ue, "supi"), string(ue, "cm_state"));
        detail_number(6, "AMF-UE-NGAP-ID", field(ue, "gnb"), "amf_ue_ngap_id");
        detail_number(6, "RAN-UE-NGAP-ID", field(ue, "gnb"), "ran_ue_ngap_id");
        detail_number(6, "PDU sessions (AMF)", ue, "pdu_sessions_count");
    }
    puts("  Observed cells (UE location reports; not a full cell inventory)");
    if (!cJSON_IsArray(cells) || !cJSON_GetArraySize(cells))
        printf("    %s\n", cJSON_IsArray(cells) ? "(none)" : "- (UE info unavailable)");
    cJSON_ArrayForEach(cell, cells) {
        char total[24], connected[24], idle[24], unknown[24];
        detail_location(4, cell);
        printf("    UE reports: total %s, connected %s, idle %s, unknown %s\n",
                decimal(cell, "ue_count", total, sizeof(total)),
                decimal(cell, "connected_ue_count", connected, sizeof(connected)),
                decimal(cell, "idle_ue_count", idle, sizeof(idle)),
                decimal(cell, "unknown_ue_count", unknown, sizeof(unknown)));
        puts("");
    }
    puts("");
}

int xcnctl_inspect(const char *kind, const xcnctl_inspect_options_t *o)
{
    bool is_ue = !strcmp(kind, "ue");
    cJSON *root = xcnctl_load_info(is_ue ? o->ue_url : o->gnb_url);
    cJSON *smf = NULL, *ues = NULL;
    cJSON *items, *item;
    int index = 0;

    if (!root) {
        fprintf(stderr, "cannot load complete AMF %s info\n", kind);
        return 1;
    }
    if (is_ue && (o->detail || o->json)) {
        smf = xcnctl_load_info(o->smf_url);
        if (!smf) fputs("warning: SMF info unavailable; UE IP, session state, QoS and N3 are unknown\n", stderr);
    }
    if (!is_ue && (o->detail || o->json)) {
        cJSON *ue;
        ues = xcnctl_load_info(o->ue_url);
        cJSON_ArrayForEach(ue, field(ues, "items")) {
            if (!prepare_ue(ue)) { cJSON_Delete(ues); ues = NULL; break; }
        }
        if (!ues) fputs("warning: UE info unavailable; gNB UE and observed-cell summaries are unknown\n", stderr);
    }
    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    while ((item = cJSON_GetArrayItem(items, index))) {
        uint64_t id;
        const cJSON *gnb;
        const char *plmn;
        if (is_ue && (!prepare_ue(item) ||
                    ((o->detail || o->json) && !enrich_sessions(item, smf)))) goto fail;
        gnb = is_ue ? ue_gnb(item) : item;
        plmn = is_ue ? string(field(field(item, "location"), "nr_cgi"), "plmn") :
            string(item, "plmn");
        if ((o->supi && strcmp(string(item, "supi"), o->supi)) ||
            (o->plmn && strcmp(plmn, o->plmn)) ||
            (o->has_gnb_id && (!number(field(gnb, "gnb_id"), UINT32_MAX, &id) ||
                id != o->gnb_id))) {
            cJSON_DeleteItemFromArray(items, index);
            continue;
        }
        if (!is_ue && (o->detail || o->json) && !enrich_gnb(item, ues)) goto fail;
        index++;
    }
    if (o->json) {
        char *json = cJSON_PrintUnformatted(root);
        if (!json) goto fail;
        puts(json);
        cJSON_free(json);
    } else {
        if (!o->detail) puts(is_ue ?
                "SUPI                     CM-STATE   PLMN   GNB-ID     GNB-LEN CELL-ID TAC      LOCATION" :
                "PLMN   GNB-ID     BITS PEER                         NG-STATE UE-COUNT TA-COUNT");
        cJSON_ArrayForEach(item, items) {
            char id[24], bits[24], cell[24], count[24], tac[24];
            if (is_ue) {
                const cJSON *loc = field(item, "location");
                if (o->detail) print_ue_detail(item);
                else printf("%-24s %-10s %-6s %-10s %-7s %-7s %-8s %s\n",
                        string(item, "supi"), string(item, "cm_state"),
                        string(field(loc, "nr_cgi"), "plmn"),
                        decimal(ue_gnb(item), "gnb_id", id, sizeof(id)),
                        decimal(field(loc, "nr_cgi"), "gnb_id_bits", bits, sizeof(bits)),
                        decimal(field(loc, "nr_cgi"), "cell_id", cell, sizeof(cell)),
                        decimal(field(loc, "nr_tai"), "tac", tac, sizeof(tac)),
                        location_label(loc));
            } else if (o->detail) {
                print_gnb_detail(item);
            } else {
                const cJSON *ng = field(item, "ng");
                const cJSON *tas = field(item, "supported_ta_list");
                char ta_count[24];
                const cJSON *setup = field(ng, "setup_success");
                const char *state = cJSON_IsTrue(setup) ? "ready" :
                    cJSON_IsFalse(setup) ? "pending" : "unknown";
                if (cJSON_IsArray(tas))
                    snprintf(ta_count, sizeof(ta_count), "%d", cJSON_GetArraySize(tas));
                else snprintf(ta_count, sizeof(ta_count), "-");
                printf("%-6s %-10s %-4s %-28s %-8s %-8s %s\n",
                        string(item, "plmn"), decimal(item, "gnb_id", id, sizeof(id)),
                        decimal(item, "gnb_id_bits", bits, sizeof(bits)),
                        string(field(ng, "sctp"), "peer"), state,
                        decimal(item, "num_connected_ues", count, sizeof(count)), ta_count);
            }
        }
    }
    cJSON_Delete(smf);
    cJSON_Delete(ues);
    cJSON_Delete(root);
    fflush(stdout);
    return 0;
fail:
    fputs("cannot build inspection output\n", stderr);
    cJSON_Delete(smf);
    cJSON_Delete(ues);
    cJSON_Delete(root);
    return 1;
}

static bool append(char *out, size_t *used, const char *format, ...)
{
    va_list ap;
    int n;
    va_start(ap, format);
    n = vsnprintf(out + *used, RATE_BYTES + 1 - *used, format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > RATE_BYTES - *used) return false;
    *used += n;
    return true;
}

char *xcnctl_add_location(const char *response, const cJSON *ues, bool json)
{
    if (json) {
        cJSON *root = cJSON_Parse(response), *row;
        char *encoded, *out;
        if (!root || !cJSON_IsArray(field(root, "rows"))) {
            cJSON_Delete(root);
            return NULL;
        }
        cJSON_ArrayForEach(row, cJSON_GetObjectItemCaseSensitive(root, "rows")) {
            const cJSON *ue = find_ue(ues, string(row, "supi"));
            if (!set(row, "location", location(ue))) {
                cJSON_Delete(root);
                return NULL;
            }
        }
        encoded = cJSON_PrintUnformatted(root);
        out = encoded ? strdup(encoded) : NULL;
        cJSON_free(encoded);
        cJSON_Delete(root);
        return out;
    }
    if (strncmp(response, "SUPI", 4)) return strdup(response);
    char *copy = strdup(response), *out = malloc(RATE_BYTES + 1);
    char *line, *save = NULL;
    size_t used = 0;
    bool header = true;
    if (!copy || !out) goto fail;
    out[0] = '\0';
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *loc;
        char *end;
        char id[24], bits[24], cell[24];
        const cJSON *ue;
        if (header) {
            if (!append(out, &used, "%s GNB-ID GNB-LEN CELL-ID GNB-PEER LOCATION\n", line)) goto fail;
            header = false;
            continue;
        }
        end = line;
        while (*end && !isspace((unsigned char)*end)) end++;
        char delimiter = *end;
        *end = '\0';
        ue = find_ue(ues, line);
        *end = delimiter;
        loc = location(ue);
        if (!loc) goto fail;
        bool ok = append(out, &used, "%s %s %s %s %s %s\n", line,
                decimal(field(loc, "gnb"), "gnb_id", id, sizeof(id)),
                decimal(field(loc, "nr_cgi"), "gnb_id_bits", bits, sizeof(bits)),
                decimal(field(loc, "nr_cgi"), "cell_id", cell, sizeof(cell)),
                string(field(loc, "gnb"), "peer"),
                location_label(loc));
        cJSON_Delete(loc);
        if (!ok) goto fail;
    }
    free(copy);
    return out;
fail:
    free(copy);
    free(out);
    return NULL;
}
