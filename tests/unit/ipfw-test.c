/* SPDX-License-Identifier: AGPL-3.0-or-later */

#include "ogs-nas-5gs.h"
#include "ipfw/ogs-ipfw.h"
#include "core/abts.h"

static void test_ipv4_qos_filter(abts_case *tc, void *data)
{
    const char *descriptions[] = {
        "permit out ip from 0.0.0.0/0 to assigned",
        "permit out ip from 192.168.1.10/32 to assigned",
        "permit out ip from any to 0.0.0.0/0",
        "permit out ip from any to 192.168.1.10/32",
    };
    const char *components[] = {
        "100000000000000000",
        "10c0a8010affffffff",
    };
    int index = OGS_POINTER_TO_UINT(data);
    ogs_ipfw_rule_t ipfw_rule, swapped, roundtrip;
    ogs_nas_qos_rule_t qos_rule, decoded;
    ogs_nas_qos_rules_t rules;
    char description[128], expected[17], hex[35];
    char *encoded;
    int rv, swap, no_local;

    ogs_cpystrn(description, descriptions[index], sizeof(description));
    rv = ogs_ipfw_compile_rule(&ipfw_rule, description);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_INT_EQUAL(tc, 1,
            index < 2 ? ipfw_rule.ipv4_src : ipfw_rule.ipv4_dst);

    /* PFCP flow descriptions must retain the explicit address/mask too. */
    encoded = ogs_ipfw_encode_flow_description(&ipfw_rule);
    ABTS_PTR_NOTNULL(tc, encoded);
    rv = ogs_ipfw_compile_rule(&roundtrip, encoded);
    ogs_free(encoded);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);
    ABTS_TRUE(tc, memcmp(&ipfw_rule, &roundtrip, sizeof(ipfw_rule)) == 0);

    for (swap = 0; swap < 2; swap++) {
        uint8_t direction = ((index < 2) != swap) ?
            OGS_FLOW_DOWNLINK_ONLY : OGS_FLOW_UPLINK_ONLY;

        swapped = ipfw_rule;
        if (swap) ogs_ipfw_rule_swap(&swapped);

        for (no_local = 0; no_local < 2; no_local++) {
            memset(&qos_rule, 0, sizeof(qos_rule));
            memset(&decoded, 0, sizeof(decoded));
            memset(&rules, 0, sizeof(rules));
            qos_rule.identifier = 2;
            qos_rule.code = OGS_NAS_QOS_CODE_CREATE_NEW_QOS_RULE;
            qos_rule.num_of_packet_filter = 1;
            qos_rule.pf[0].identifier = 1;
            qos_rule.pf[0].direction = direction;
            qos_rule.precedence = 100;
            qos_rule.flow.identifier = 2;
            ogs_pf_content_from_ipfw_rule(direction,
                    &qos_rule.pf[0].content, &swapped, no_local);
            ABTS_INT_EQUAL(tc, 9, qos_rule.pf[0].content.length);
            ABTS_INT_EQUAL(tc, 1, qos_rule.pf[0].content.num_of_component);

            /* Compare the wire bytes, including lengths and network byte order. */
            ogs_snprintf(hex, sizeof(hex), "02000e21%02x09%s6402",
                    (direction << 4) | 1, components[index % 2]);
            ogs_hex_from_string(hex, expected, sizeof(expected));
            rv = ogs_nas_build_qos_rules(&rules, &qos_rule, 1);
            ABTS_INT_EQUAL(tc, OGS_OK, rv);
            ABTS_INT_EQUAL(tc, sizeof(expected), rules.length);
            if (rules.length == sizeof(expected))
                ABTS_TRUE(tc, memcmp(expected, rules.buffer, sizeof(expected)) == 0);
            rv = ogs_nas_parse_qos_rules(&decoded, &rules);
            ABTS_INT_EQUAL(tc, 1, rv);
            ABTS_INT_EQUAL(tc, 9, decoded.pf[0].content.length);
            ogs_free(rules.buffer);
        }
    }
}

abts_suite *test_ipfw(abts_suite *suite)
{
    int i;

    suite = ADD_SUITE(suite);
    for (i = 0; i < 4; i++)
        abts_run_test(suite, test_ipv4_qos_filter, OGS_UINT_TO_POINTER(i));
    return suite;
}
