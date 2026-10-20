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
        if (number(field(session, "psi"), 255, &psi) && psi) {
            cJSON_ArrayForEach(pdu, field(smf_ue, "pdu")) {
                if (number(field(pdu, "psi"), 255, &candidate) && candidate == psi) {
                    if (found) { ambiguous = true; break; }
                    found = pdu;
                }
            }
        }
        if (ambiguous || (found && !session_compatible(session, found))) found = NULL;
        const char *keys[] = {"ipv4", "ipv6", "pdu_state"};
        size_t i;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const cJSON *v = field(found, keys[i]);
            if (!set(session, keys[i], cJSON_IsString(v) ?
                        cJSON_Duplicate(v, true) : cJSON_CreateNull())) return false;
        }
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

static void detail_json(const char *label, const cJSON *value)
{
    char *json = value ? cJSON_Print(value) : NULL;
    printf("  %s: %s\n", label, json ? json : "-");
    cJSON_free(json);
}

static void print_ue_detail(const cJSON *ue)
{
    const cJSON *loc = field(ue, "location");
    const cJSON *cgi = field(loc, "nr_cgi");
    const cJSON *gnb = field(loc, "gnb");
    const cJSON *session;
    char id[24], derived_id[24], bits[24], cell[24], nci[24];
    uint64_t timestamp;

    printf("UE %s\n  CM-STATE: %s\n  NR CGI: %s-%s\n"
            "  NCI: %s (%s)\n"
            "  gNB ID (reported): %s\n  gNB ID (from NCI): %s\n"
            "  gNB ID length: %s bits\n  Cell local ID: %s\n"
            "  gNB PLMN: %s\n  gNB peer: %s\n  Location: %s\n",
            string(ue, "supi"), string(ue, "cm_state"), string(cgi, "plmn"),
            string(cgi, "nci_hex"), decimal(cgi, "nci", nci, sizeof(nci)),
            string(cgi, "nci_hex"), decimal(gnb, "gnb_id", id, sizeof(id)),
            decimal(cgi, "gnb_id", derived_id, sizeof(derived_id)),
            decimal(cgi, "gnb_id_bits", bits, sizeof(bits)),
            decimal(cgi, "cell_id", cell, sizeof(cell)), string(gnb, "plmn"),
            string(gnb, "peer"), string(loc, "status"));
    if (cJSON_IsFalse(field(cgi, "identity_consistent")))
        puts("  Identity: identity-mismatch (NCI prefix does not match the reported gNB ID)");
    if (number(field(loc, "timestamp"), 9007199254740991ULL, &timestamp) && timestamp) {
        time_t seconds = timestamp / 1000000;
        time_t now = time(NULL);
        struct tm utc;
        char date[32];
        if (gmtime_r(&seconds, &utc) &&
            strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%SZ", &utc))
            printf("  Location updated: %s (age %.0f s)\n", date,
                    difftime(now, seconds) > 0 ? difftime(now, seconds) : 0);
    } else puts("  Location updated: -");
    detail_json("Current RAN association", field(ue, "gnb"));
    detail_json("Location gNB snapshot", field(loc, "gnb"));
    detail_json("TAI", field(loc, "nr_tai"));
    detail_json("Security algorithms", field(ue, "security"));
    cJSON_ArrayForEach(session, field(ue, "pdu_sessions"))
        detail_json("PDU Session", session);
    puts("");
}

static void print_gnb_detail(const cJSON *gnb)
{
    const cJSON *ng = field(gnb, "ng");
    const cJSON *sctp = field(ng, "sctp");
    const cJSON *setup = field(ng, "setup_success");
    const cJSON *tas = field(gnb, "supported_ta_list");
    const cJSON *ta;
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
    printf("    %-16s: %s\n", "PEER", string(sctp, "peer"));
    printf("    %-16s: %s\n", "MAX-OUT-STREAMS",
            decimal(sctp, "max_out_streams", value, sizeof(value)));
    printf("    %-16s: %s\n", "NEXT-STREAM-ID",
            decimal(sctp, "next_ostream_id", value, sizeof(value)));
    puts("  Supported TA/PLMN/slices");
    if (!cJSON_IsArray(tas) || !cJSON_GetArraySize(tas)) {
        printf("    %s\n\n", cJSON_IsArray(tas) ? "(none)" : "-");
        return;
    }
    cJSON_ArrayForEach(ta, tas) {
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
    puts("");
}

int xcnctl_inspect(const char *kind, const xcnctl_inspect_options_t *o)
{
    bool is_ue = !strcmp(kind, "ue");
    cJSON *root = xcnctl_load_info(is_ue ? o->ue_url : o->gnb_url);
    cJSON *smf = NULL;
    cJSON *items, *item;
    int index = 0;

    if (!root) {
        fprintf(stderr, "cannot load complete AMF %s info\n", kind);
        return 1;
    }
    if (is_ue && (o->detail || o->json)) {
        smf = xcnctl_load_info(o->smf_url);
        if (!smf) fputs("warning: SMF info unavailable; UE IP and session state are unknown\n", stderr);
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
    cJSON_Delete(root);
    fflush(stdout);
    return 0;
fail:
    fputs("cannot build inspection output\n", stderr);
    cJSON_Delete(smf);
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
