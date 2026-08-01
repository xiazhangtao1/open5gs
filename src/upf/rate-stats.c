/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "rate-stats.h"
#include "dataplane.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define UPF_RATE_MAX_DIRTY (UPF_DATAPLANE_MAX_BURST * 2)
#define UPF_RATE_REQUEST_MAX 512
#define UPF_RATE_RESPONSE_MAX (4 * 1024 * 1024)

typedef struct {
    const char *level;
    const char *supi;
    const char *ue_ip;
    uint64_t seid;
    bool seid_present;
    bool json;
} upf_rate_query_t;

typedef struct {
    char supi[OGS_MAX_IMSI_BCD_LEN + 6];
    uint64_t ul_bps;
    uint64_t dl_bps;
    uint64_t ul_pps;
    uint64_t dl_pps;
    uint64_t ul_octets;
    uint64_t dl_octets;
    uint64_t ul_packets;
    uint64_t dl_packets;
} upf_rate_user_t;

static ogs_thread_t *rate_thread;
static int listen_fd = -1;
static int stopping;
static __thread upf_rate_slot_t *dirty_slot[UPF_RATE_MAX_DIRTY];
static __thread uint16_t dirty_count;
static __thread uint32_t dirty_epoch;
static __thread bool batch_active;

static bool rate_is_stopping(void)
{
    return __atomic_load_n(&stopping, __ATOMIC_ACQUIRE);
}

void upf_rate_stats_batch_begin(void)
{
    if (!upf_self()->rate_stats.enabled)
        return;
    dirty_count = 0;
    if (++dirty_epoch == 0)
        dirty_epoch = 1;
    batch_active = true;
}

void upf_rate_stats_record(upf_rate_slot_t *slot, uint32_t octets)
{
    if (!upf_self()->rate_stats.enabled || !slot || !slot->active)
        return;

    slot->live_octets += octets;
    slot->live_packets++;
    if (!batch_active) {
        __atomic_store_n(&slot->published_octets,
                slot->live_octets, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->published_packets,
                slot->live_packets, __ATOMIC_RELEASE);
        return;
    }
    if (slot->dirty_epoch != dirty_epoch &&
        dirty_count < OGS_ARRAY_SIZE(dirty_slot)) {
        slot->dirty_epoch = dirty_epoch;
        dirty_slot[dirty_count++] = slot;
    }
}

void upf_rate_stats_batch_end(void)
{
    uint16_t i;

    if (!upf_self()->rate_stats.enabled)
        return;
    for (i = 0; i < dirty_count; i++) {
        upf_rate_slot_t *slot = dirty_slot[i];

        __atomic_store_n(&slot->published_octets,
                slot->live_octets, __ATOMIC_RELEASE);
        __atomic_store_n(&slot->published_packets,
                slot->live_packets, __ATOMIC_RELEASE);
    }
    dirty_count = 0;
    batch_active = false;
}

void upf_rate_stats_set_user(
        upf_sess_t *sess, const ogs_pfcp_tlv_user_id_t *user_id)
{
    const uint8_t *p;
    size_t remaining;
    ogs_pfcp_user_id_flags_t flags;
    uint8_t len;
    char imsi[OGS_MAX_IMSI_BCD_LEN + 1];

    ogs_assert(sess);
    sess->supi[0] = '\0';
    if (!user_id || !user_id->presence || !user_id->data ||
        user_id->len < 2)
        return;

    p = user_id->data;
    remaining = user_id->len;
    flags.flags = *p++;
    remaining--;
    if (!flags.imsif || !remaining)
        return;
    len = *p++;
    remaining--;
    if (!len || len > OGS_MAX_IMSI_LEN || len > remaining)
        return;

    memset(imsi, 0, sizeof(imsi));
    ogs_buffer_to_bcd((uint8_t *)p, len, imsi);
    ogs_snprintf(sess->supi, sizeof(sess->supi), "imsi-%s", imsi);
}

void upf_rate_stats_sync_rules(upf_sess_t *sess)
{
    bool present[OGS_MAX_NUM_OF_PDR] = { false };
    ogs_pfcp_pdr_t *pdr;
    int i;

    ogs_assert(sess);
    if (!upf_self()->rate_stats.enabled)
        return;

    ogs_list_for_each(&sess->pfcp.pdr_list, pdr) {
        upf_rate_slot_t *slot;
        uint32_t qer_id = pdr->qer ? pdr->qer->id : 0;
        uint8_t qfi = pdr->qer ? pdr->qer->qfi : pdr->qfi;
        uint8_t direction = pdr->src_if == OGS_PFCP_INTERFACE_ACCESS ?
            UPF_RATE_DIR_UL : UPF_RATE_DIR_DL;

        if (!pdr->id || pdr->id > OGS_MAX_NUM_OF_PDR)
            continue;
        slot = &sess->rate_slot[pdr->id - 1];
        present[pdr->id - 1] = true;
        if (slot->active && slot->pdr_id == pdr->id &&
            slot->qer_id == qer_id && slot->qfi == qfi &&
            slot->direction == direction)
            continue;
        {
            uint32_t generation = slot->generation + 1;

            memset(slot, 0, sizeof(*slot));
            slot->generation = generation;
        }
        slot->active = true;
        slot->pdr_id = pdr->id;
        slot->qer_id = qer_id;
        slot->qfi = qfi;
        slot->direction = direction;
    }

    for (i = 0; i < OGS_MAX_NUM_OF_PDR; i++) {
        if (!present[i] && sess->rate_slot[i].active) {
            uint32_t generation = sess->rate_slot[i].generation + 1;
            memset(&sess->rate_slot[i], 0, sizeof(sess->rate_slot[i]));
            sess->rate_slot[i].generation = generation;
        }
    }
}

upf_rate_slot_t *upf_rate_stats_slot(
        upf_sess_t *sess, const ogs_pfcp_pdr_t *pdr)
{
    if (!upf_self()->rate_stats.enabled || !sess || !pdr ||
        !pdr->id || pdr->id > OGS_MAX_NUM_OF_PDR)
        return NULL;
    return &sess->rate_slot[pdr->id - 1];
}

static void sample_rates(ogs_time_t now, ogs_time_t *previous)
{
    upf_sess_t *sess;
    uint64_t elapsed;

    if (!*previous) {
        *previous = now;
        return;
    }
    elapsed = now > *previous ? now - *previous : 1;
    upf_dataplane_read_lock();
    ogs_list_for_each(&upf_self()->sess_list, sess) {
        int i;

        for (i = 0; i < OGS_MAX_NUM_OF_PDR; i++) {
            upf_rate_slot_t *slot = &sess->rate_slot[i];
            uint64_t octets;
            uint64_t packets;

            if (!slot->active)
                continue;
            octets = __atomic_load_n(
                    &slot->published_octets, __ATOMIC_ACQUIRE);
            packets = __atomic_load_n(
                    &slot->published_packets, __ATOMIC_ACQUIRE);
            slot->rate_bps = (octets - slot->sampled_octets) * 8 *
                OGS_USEC_PER_SEC / elapsed;
            slot->rate_pps = (packets - slot->sampled_packets) *
                OGS_USEC_PER_SEC / elapsed;
            slot->sampled_octets = octets;
            slot->sampled_packets = packets;
        }
    }
    upf_dataplane_read_unlock();
    *previous = now;
}

static void session_ip(const upf_sess_t *sess, char *buf, size_t size)
{
    if (sess->ipv4)
        inet_ntop(AF_INET, sess->ipv4->addr, buf, size);
    else if (sess->ipv6)
        inet_ntop(AF_INET6, sess->ipv6->addr, buf, size);
    else
        ogs_cpystrn(buf, "-", size);
}

static bool query_matches(
        const upf_rate_query_t *query, const upf_sess_t *sess,
        const char *ip)
{
    if (query->supi && strcmp(query->supi,
                sess->supi[0] ? sess->supi : "unknown"))
        return false;
    if (query->ue_ip && strcmp(query->ue_ip, ip))
        return false;
    if (query->seid_present && query->seid != sess->upf_n4_seid)
        return false;
    return true;
}

static size_t appendf(char *buf, size_t used, size_t size,
        const char *format, ...)
{
    va_list ap;
    int written;

    if (used >= size)
        return used;
    va_start(ap, format);
    written = vsnprintf(buf + used, size - used, format, ap);
    va_end(ap);
    if (written < 0)
        return used;
    if ((size_t)written >= size - used)
        return size;
    return used + written;
}

static void slot_add(const upf_rate_slot_t *slot,
        uint64_t *ul_bps, uint64_t *dl_bps,
        uint64_t *ul_pps, uint64_t *dl_pps,
        uint64_t *ul_octets, uint64_t *dl_octets,
        uint64_t *ul_packets, uint64_t *dl_packets)
{
    uint64_t octets = __atomic_load_n(
            &slot->published_octets, __ATOMIC_ACQUIRE);
    uint64_t packets = __atomic_load_n(
            &slot->published_packets, __ATOMIC_ACQUIRE);

    if (slot->direction == UPF_RATE_DIR_UL) {
        *ul_bps += slot->rate_bps;
        *ul_pps += slot->rate_pps;
        *ul_octets += octets;
        *ul_packets += packets;
    } else {
        *dl_bps += slot->rate_bps;
        *dl_pps += slot->rate_pps;
        *dl_octets += octets;
        *dl_packets += packets;
    }
}

static size_t render_session_rows(
        char *buf, size_t size, const upf_rate_query_t *query)
{
    upf_sess_t *sess;
    size_t used = 0;
    bool first = true;

    if (query->json)
        used = appendf(buf, used, size, "{\"level\":\"%s\",\"rows\":[",
                query->level);
    else
        used = appendf(buf, used, size,
                "SUPI UE-IP SEID DNN WORKER UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes\n");

    upf_dataplane_read_lock();
    ogs_list_for_each(&upf_self()->sess_list, sess) {
        char ip[INET6_ADDRSTRLEN];
        uint64_t ul_bps = 0, dl_bps = 0, ul_pps = 0, dl_pps = 0;
        uint64_t ul_octets = 0, dl_octets = 0;
        uint64_t ul_packets = 0, dl_packets = 0;
        int i;

        session_ip(sess, ip, sizeof(ip));
        if (!query_matches(query, sess, ip))
            continue;
        for (i = 0; i < OGS_MAX_NUM_OF_PDR; i++)
            if (sess->rate_slot[i].active)
                slot_add(&sess->rate_slot[i],
                        &ul_bps, &dl_bps, &ul_pps, &dl_pps,
                        &ul_octets, &dl_octets, &ul_packets, &dl_packets);
        if (query->json) {
            used = appendf(buf, used, size,
                    "%s{\"supi\":\"%s\",\"ue_ip\":\"%s\","
                    "\"seid\":%llu,\"dnn\":\"%s\",\"worker\":%u,"
                    "\"ul_bps\":%llu,\"dl_bps\":%llu,"
                    "\"ul_pps\":%llu,\"dl_pps\":%llu,"
                    "\"ul_bytes\":%llu,\"dl_bytes\":%llu}",
                    first ? "" : ",", sess->supi[0] ? sess->supi : "unknown",
                    ip, (unsigned long long)sess->upf_n4_seid,
                    sess->apn_dnn ? sess->apn_dnn : "-", sess->owner_worker,
                    (unsigned long long)ul_bps, (unsigned long long)dl_bps,
                    (unsigned long long)ul_pps, (unsigned long long)dl_pps,
                    (unsigned long long)ul_octets,
                    (unsigned long long)dl_octets);
        } else {
            used = appendf(buf, used, size,
                    "%s %s %llu %s %u %.3f %.3f %llu %llu %llu %llu\n",
                    sess->supi[0] ? sess->supi : "unknown", ip,
                    (unsigned long long)sess->upf_n4_seid,
                    sess->apn_dnn ? sess->apn_dnn : "-", sess->owner_worker,
                    (double)ul_bps / 1000000.0,
                    (double)dl_bps / 1000000.0,
                    (unsigned long long)ul_pps,
                    (unsigned long long)dl_pps,
                    (unsigned long long)ul_octets,
                    (unsigned long long)dl_octets);
        }
        first = false;
    }
    upf_dataplane_read_unlock();
    if (query->json)
        used = appendf(buf, used, size, "]}\n");
    return used;
}

static size_t render_rule_rows(
        char *buf, size_t size, const upf_rate_query_t *query)
{
    upf_sess_t *sess;
    size_t used = 0;
    bool first = true;

    if (query->json)
        used = appendf(buf, used, size, "{\"level\":\"rule\",\"rows\":[");
    else
        used = appendf(buf, used, size,
                "SUPI UE-IP SEID QFI PDR QER DIR Mbps pps bytes packets\n");
    upf_dataplane_read_lock();
    ogs_list_for_each(&upf_self()->sess_list, sess) {
        char ip[INET6_ADDRSTRLEN];
        int i;

        session_ip(sess, ip, sizeof(ip));
        if (!query_matches(query, sess, ip))
            continue;
        for (i = 0; i < OGS_MAX_NUM_OF_PDR; i++) {
            upf_rate_slot_t *slot = &sess->rate_slot[i];
            uint64_t octets;
            uint64_t packets;

            if (!slot->active)
                continue;
            octets = __atomic_load_n(
                    &slot->published_octets, __ATOMIC_ACQUIRE);
            packets = __atomic_load_n(
                    &slot->published_packets, __ATOMIC_ACQUIRE);
            if (query->json) {
                used = appendf(buf, used, size,
                        "%s{\"supi\":\"%s\",\"ue_ip\":\"%s\","
                        "\"seid\":%llu,\"qfi\":%u,\"pdr_id\":%u,"
                        "\"qer_id\":%u,\"direction\":\"%s\","
                        "\"bps\":%llu,\"pps\":%llu,"
                        "\"bytes\":%llu,\"packets\":%llu}",
                        first ? "" : ",",
                        sess->supi[0] ? sess->supi : "unknown", ip,
                        (unsigned long long)sess->upf_n4_seid,
                        slot->qfi, slot->pdr_id, slot->qer_id,
                        slot->direction == UPF_RATE_DIR_UL ? "UL" : "DL",
                        (unsigned long long)slot->rate_bps,
                        (unsigned long long)slot->rate_pps,
                        (unsigned long long)octets,
                        (unsigned long long)packets);
            } else {
                used = appendf(buf, used, size,
                        "%s %s %llu %u %u %u %s %.3f %llu %llu %llu\n",
                        sess->supi[0] ? sess->supi : "unknown", ip,
                        (unsigned long long)sess->upf_n4_seid, slot->qfi,
                        slot->pdr_id, slot->qer_id,
                        slot->direction == UPF_RATE_DIR_UL ? "UL" : "DL",
                        (double)slot->rate_bps / 1000000.0,
                        (unsigned long long)slot->rate_pps,
                        (unsigned long long)octets,
                        (unsigned long long)packets);
            }
            first = false;
        }
    }
    upf_dataplane_read_unlock();
    if (query->json)
        used = appendf(buf, used, size, "]}\n");
    return used;
}

static size_t render_bearer_rows(
        char *buf, size_t size, const upf_rate_query_t *query)
{
    upf_sess_t *sess;
    size_t used = 0;
    bool first = true;

    if (query->json)
        used = appendf(buf, used, size,
                "{\"level\":\"bearer\",\"rows\":[");
    else
        used = appendf(buf, used, size,
                "SUPI UE-IP SEID QFI UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes UL-packets DL-packets\n");
    upf_dataplane_read_lock();
    ogs_list_for_each(&upf_self()->sess_list, sess) {
        char ip[INET6_ADDRSTRLEN];
        int qfi;

        session_ip(sess, ip, sizeof(ip));
        if (!query_matches(query, sess, ip))
            continue;
        for (qfi = 0; qfi < 64; qfi++) {
            uint64_t ul_bps = 0, dl_bps = 0, ul_pps = 0, dl_pps = 0;
            uint64_t ul_octets = 0, dl_octets = 0;
            uint64_t ul_packets = 0, dl_packets = 0;
            bool present = false;
            int i;

            for (i = 0; i < OGS_MAX_NUM_OF_PDR; i++) {
                upf_rate_slot_t *slot = &sess->rate_slot[i];

                if (!slot->active || slot->qfi != qfi)
                    continue;
                present = true;
                slot_add(slot, &ul_bps, &dl_bps, &ul_pps, &dl_pps,
                        &ul_octets, &dl_octets, &ul_packets, &dl_packets);
            }
            if (!present)
                continue;
            if (query->json)
                used = appendf(buf, used, size,
                        "%s{\"supi\":\"%s\",\"ue_ip\":\"%s\","
                        "\"seid\":%llu,\"qfi\":%d,"
                        "\"ul_bps\":%llu,\"dl_bps\":%llu,"
                        "\"ul_pps\":%llu,\"dl_pps\":%llu,"
                        "\"ul_bytes\":%llu,\"dl_bytes\":%llu,"
                        "\"ul_packets\":%llu,\"dl_packets\":%llu}",
                        first ? "" : ",",
                        sess->supi[0] ? sess->supi : "unknown", ip,
                        (unsigned long long)sess->upf_n4_seid, qfi,
                        (unsigned long long)ul_bps,
                        (unsigned long long)dl_bps,
                        (unsigned long long)ul_pps,
                        (unsigned long long)dl_pps,
                        (unsigned long long)ul_octets,
                        (unsigned long long)dl_octets,
                        (unsigned long long)ul_packets,
                        (unsigned long long)dl_packets);
            else
                used = appendf(buf, used, size,
                        "%s %s %llu %d %.3f %.3f %llu %llu %llu %llu %llu %llu\n",
                        sess->supi[0] ? sess->supi : "unknown", ip,
                        (unsigned long long)sess->upf_n4_seid, qfi,
                        (double)ul_bps / 1000000.0,
                        (double)dl_bps / 1000000.0,
                        (unsigned long long)ul_pps,
                        (unsigned long long)dl_pps,
                        (unsigned long long)ul_octets,
                        (unsigned long long)dl_octets,
                        (unsigned long long)ul_packets,
                        (unsigned long long)dl_packets);
            first = false;
        }
    }
    upf_dataplane_read_unlock();
    if (query->json)
        used = appendf(buf, used, size, "]}\n");
    return used;
}

static size_t render_user_rows(
        char *buf, size_t size, const upf_rate_query_t *query)
{
    upf_rate_user_t *users;
    upf_sess_t *sess;
    size_t used = 0;
    int count = 0;
    int i;

    users = ogs_calloc(ogs_app()->pool.sess, sizeof(*users));
    if (!users)
        return appendf(buf, used, size, "ERROR out of memory\n");
    upf_dataplane_read_lock();
    ogs_list_for_each(&upf_self()->sess_list, sess) {
        char ip[INET6_ADDRSTRLEN];
        const char *supi = sess->supi[0] ? sess->supi : "unknown";
        int user = -1;
        int j;

        session_ip(sess, ip, sizeof(ip));
        if (!query_matches(query, sess, ip))
            continue;
        for (j = 0; j < count; j++)
            if (!strcmp(users[j].supi, supi)) {
                user = j;
                break;
            }
        if (user < 0 && count < ogs_app()->pool.sess) {
            user = count++;
            ogs_cpystrn(users[user].supi, supi, sizeof(users[user].supi));
        }
        if (user < 0)
            continue;
        for (j = 0; j < OGS_MAX_NUM_OF_PDR; j++)
            if (sess->rate_slot[j].active)
                slot_add(&sess->rate_slot[j],
                        &users[user].ul_bps, &users[user].dl_bps,
                        &users[user].ul_pps, &users[user].dl_pps,
                        &users[user].ul_octets, &users[user].dl_octets,
                        &users[user].ul_packets, &users[user].dl_packets);
    }
    upf_dataplane_read_unlock();

    if (query->json)
        used = appendf(buf, used, size, "{\"level\":\"user\",\"rows\":[");
    else
        used = appendf(buf, used, size,
                "SUPI UL-Mbps DL-Mbps UL-pps DL-pps UL-bytes DL-bytes\n");
    for (i = 0; i < count; i++) {
        if (query->json)
            used = appendf(buf, used, size,
                    "%s{\"supi\":\"%s\",\"ul_bps\":%llu,"
                    "\"dl_bps\":%llu,\"ul_pps\":%llu,"
                    "\"dl_pps\":%llu,\"ul_bytes\":%llu,"
                    "\"dl_bytes\":%llu}", i ? "," : "", users[i].supi,
                    (unsigned long long)users[i].ul_bps,
                    (unsigned long long)users[i].dl_bps,
                    (unsigned long long)users[i].ul_pps,
                    (unsigned long long)users[i].dl_pps,
                    (unsigned long long)users[i].ul_octets,
                    (unsigned long long)users[i].dl_octets);
        else
            used = appendf(buf, used, size,
                    "%s %.3f %.3f %llu %llu %llu %llu\n", users[i].supi,
                    (double)users[i].ul_bps / 1000000.0,
                    (double)users[i].dl_bps / 1000000.0,
                    (unsigned long long)users[i].ul_pps,
                    (unsigned long long)users[i].dl_pps,
                    (unsigned long long)users[i].ul_octets,
                    (unsigned long long)users[i].dl_octets);
    }
    if (query->json)
        used = appendf(buf, used, size, "]}\n");
    ogs_free(users);
    return used;
}

static void parse_query(char *request, upf_rate_query_t *query)
{
    char *save = NULL;
    char *token;

    memset(query, 0, sizeof(*query));
    query->level = "session";
    for (token = strtok_r(request, " \t\r\n", &save); token;
            token = strtok_r(NULL, " \t\r\n", &save)) {
        if (!strncmp(token, "level=", 6))
            query->level = token + 6;
        else if (!strncmp(token, "supi=", 5))
            query->supi = token + 5;
        else if (!strncmp(token, "ue_ip=", 6))
            query->ue_ip = token + 6;
        else if (!strncmp(token, "seid=", 5)) {
            char *end = NULL;
            query->seid = strtoull(token + 5, &end, 0);
            query->seid_present = end && *end == '\0';
        } else if (!strcmp(token, "json=1"))
            query->json = true;
    }
}

static void serve_client(int fd)
{
    char request[UPF_RATE_REQUEST_MAX];
    char *response;
    upf_rate_query_t query;
    ssize_t length;
    size_t used;

    length = read(fd, request, sizeof(request) - 1);
    if (length <= 0)
        return;
    request[length] = '\0';
    parse_query(request, &query);
    response = ogs_malloc(UPF_RATE_RESPONSE_MAX);
    if (!response)
        return;
    if (!strcmp(query.level, "user"))
        used = render_user_rows(response, UPF_RATE_RESPONSE_MAX, &query);
    else if (!strcmp(query.level, "bearer"))
        used = render_bearer_rows(response, UPF_RATE_RESPONSE_MAX, &query);
    else if (!strcmp(query.level, "rule"))
        used = render_rule_rows(response, UPF_RATE_RESPONSE_MAX, &query);
    else if (!strcmp(query.level, "session"))
        used = render_session_rows(response, UPF_RATE_RESPONSE_MAX, &query);
    else
        used = appendf(response, 0, UPF_RATE_RESPONSE_MAX,
                "ERROR invalid level; expected user, session, bearer or rule\n");
    if (used > UPF_RATE_RESPONSE_MAX)
        used = UPF_RATE_RESPONSE_MAX;
    {
        size_t offset = 0;

        while (offset < used) {
            ssize_t written = write(fd, response + offset, used - offset);

            if (written > 0) {
                offset += written;
                continue;
            }
            if (written < 0 && errno == EINTR)
                continue;
            if (written < 0)
                ogs_debug("UPF rate CLI write failed: %s", strerror(errno));
            break;
        }
    }
    ogs_free(response);
}

static void rate_main(void *data)
{
    ogs_time_t previous = 0;
    ogs_time_t next_sample = 0;

    (void)data;
    upf_dataplane_pin_control_thread("rate/control");
    while (!rate_is_stopping()) {
        struct pollfd pfd = { .fd = listen_fd, .events = POLLIN };
        ogs_time_t now = ogs_get_monotonic_time();
        int timeout = 100;
        int rv;

        if (!next_sample || now >= next_sample) {
            sample_rates(now, &previous);
            next_sample = now +
                (ogs_time_t)upf_self()->rate_stats.interval_ms * 1000;
        } else {
            uint64_t remaining = (next_sample - now + 999) / 1000;
            if (remaining < (uint64_t)timeout)
                timeout = remaining;
        }
        rv = poll(&pfd, 1, timeout);
        if (rv > 0 && (pfd.revents & POLLIN)) {
            int client = accept(listen_fd, NULL, NULL);
            if (client >= 0) {
                serve_client(client);
                close(client);
            }
        } else if (rv < 0 && errno != EINTR && !rate_is_stopping()) {
            ogs_warn("UPF rate CLI poll failed: %s", strerror(errno));
        }
    }
}

int upf_rate_stats_open(void)
{
    struct sockaddr_un address;
    char directory[sizeof(address.sun_path)];
    char *slash;

    if (!upf_self()->rate_stats.enabled)
        return OGS_OK;
    if (strlen(upf_self()->rate_stats.socket_path) >= sizeof(address.sun_path)) {
        ogs_error("UPF rate socket path is too long");
        return OGS_ERROR;
    }
    ogs_cpystrn(directory, upf_self()->rate_stats.socket_path,
            sizeof(directory));
    slash = strrchr(directory, '/');
    if (slash && slash != directory) {
        *slash = '\0';
        if (mkdir(directory, 0755) < 0 && errno != EEXIST) {
            ogs_error("Cannot create UPF rate socket directory: %s",
                    strerror(errno));
            return OGS_ERROR;
        }
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0)
        return OGS_ERROR;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    ogs_cpystrn(address.sun_path, upf_self()->rate_stats.socket_path,
            sizeof(address.sun_path));
    unlink(address.sun_path);
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        chmod(address.sun_path, 0600) < 0 || listen(listen_fd, 8) < 0) {
        ogs_error("Cannot open UPF rate socket %s: %s",
                address.sun_path, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        unlink(address.sun_path);
        return OGS_ERROR;
    }
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
    rate_thread = ogs_thread_create(rate_main, NULL);
    if (!rate_thread) {
        close(listen_fd);
        listen_fd = -1;
        unlink(address.sun_path);
        return OGS_ERROR;
    }
    ogs_info("UPF rate CLI listening on %s [interval:%ums]",
            address.sun_path, upf_self()->rate_stats.interval_ms);
    return OGS_OK;
}

void upf_rate_stats_close(void)
{
    if (!upf_self()->rate_stats.enabled)
        return;
    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
    if (rate_thread) {
        ogs_thread_destroy(rate_thread);
        rate_thread = NULL;
    }
    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }
    unlink(upf_self()->rate_stats.socket_path);
}
