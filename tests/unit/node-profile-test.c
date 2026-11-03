/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/amf/node-profile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "node profile check failed at line %d: %s\n", __LINE__, #x); \
    exit(1); } } while (0)

static xcn_auth_payload_t item(uint64_t timestamp, uint64_t random)
{
    xcn_auth_payload_t payload;
    unsigned int i;
    payload.timestamp = timestamp;
    for (i = 0; i < 8; i++)
        payload.random[i] = (uint8_t)(random >> (i * 8));
    return payload;
}

int main(void)
{
    const uint64_t now = 1700000000;
    xcn_auth_payload_t payload, first, latest;
    uint64_t i;
    char token[XCN_AUTH_RAN_NAME_LEN + 1];
    time_t current;

    amf_node_profile_init();
    CHECK(amf_node_profile_check_payload(NULL, now) == XCN_AUTH_ERR_PARAM);
    payload = item(now - AMF_NODE_PROFILE_WINDOW, 1);
    CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_OK);
    payload = item(now + AMF_NODE_PROFILE_WINDOW, 2);
    CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_OK);
    payload = item(now - AMF_NODE_PROFILE_WINDOW - 1, 3);
    CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_ERR_TIME);
    payload = item(now + AMF_NODE_PROFILE_WINDOW + 1, 4);
    CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_ERR_TIME);
    payload = item(UINT64_MAX, 5);
    CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_ERR_TIME);
    payload = item(0, 6);
    CHECK(amf_node_profile_check_payload(&payload, UINT64_MAX) == XCN_AUTH_ERR_TIME);

    amf_node_profile_init();
    first = item(now + AMF_NODE_PROFILE_WINDOW, 7);
    CHECK(amf_node_profile_check_payload(&first, now) == XCN_AUTH_OK);
    CHECK(amf_node_profile_check_payload(&first,
                now + 2 * AMF_NODE_PROFILE_WINDOW) == XCN_AUTH_ERR_REPLAY);
    latest = item(now + 2 * AMF_NODE_PROFILE_WINDOW + 1, 8);
    CHECK(amf_node_profile_check_payload(&latest, latest.timestamp) == XCN_AUTH_OK);
    CHECK(amf_node_profile_count() == 1);

    /* A rollback does not impose a high-water mark or clear live entries. */
    amf_node_profile_init();
    first = item(now, 10);
    CHECK(amf_node_profile_check_payload(&first, now) == XCN_AUTH_OK);
    latest = item(now - 3600, 11);
    CHECK(amf_node_profile_check_payload(&latest, now - 3600) == XCN_AUTH_OK);
    CHECK(amf_node_profile_check_payload(&first, now - 3600) == XCN_AUTH_ERR_REPLAY);
    latest = item(now + 10, 10);
    CHECK(amf_node_profile_check_payload(&latest, now) == XCN_AUTH_ERR_REPLAY);

    amf_node_profile_init();
    for (i = 0; i <= AMF_NODE_PROFILE_CAPACITY; i++) {
        payload = item(now, i);
        CHECK(amf_node_profile_check_payload(&payload, now) == XCN_AUTH_OK);
    }
    CHECK(amf_node_profile_count() == AMF_NODE_PROFILE_CAPACITY);
    latest = item(now, AMF_NODE_PROFILE_CAPACITY);
    CHECK(amf_node_profile_check_payload(&latest, now) == XCN_AUTH_ERR_REPLAY);
    first = item(now, 0);
    CHECK(amf_node_profile_check_payload(&first, now) == XCN_AUTH_OK);
    CHECK(amf_node_profile_count() == AMF_NODE_PROFILE_CAPACITY);
    first = item(now, 1);
    CHECK(amf_node_profile_check_payload(&first, now) == XCN_AUTH_OK);

    /* Purged tokens may be accepted after a clock rollback, by policy. */
    latest = item(now + AMF_NODE_PROFILE_WINDOW + 1, UINT64_MAX);
    CHECK(amf_node_profile_check_payload(&latest, latest.timestamp) == XCN_AUTH_OK);
    CHECK(amf_node_profile_count() == 1);
    CHECK(amf_node_profile_check_payload(&first, now) == XCN_AUTH_OK);

    amf_node_profile_init();
    CHECK(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    current = time(NULL);
    CHECK(current >= 0);
    CHECK(amf_node_profile_check(token, 56, (uint64_t)current) == XCN_AUTH_OK);
    CHECK(amf_node_profile_check(token, 56, (uint64_t)current) == XCN_AUTH_ERR_REPLAY);
    token[0] = '!';
    CHECK(amf_node_profile_check(token, 56, (uint64_t)current) == XCN_AUTH_ERR_FORMAT);
    amf_node_profile_init();
    CHECK(amf_node_profile_count() == 0);
    puts("node profile: window, rollback, replay, FIFO capacity and expiry passed");
    return 0;
}
