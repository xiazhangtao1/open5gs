/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/amf/context.h"
#include "../../src/amf/ngap-handler.h"
#include "../../src/amf/node-profile.h"
#include "node-profile-fixture.h"
#include <time.h>

static int setup_result;
static long setup_cause;
static time_t test_clock = 1700000000;

time_t __wrap_time(time_t *out);
int __wrap_ngap_send_ng_setup_response(amf_gnb_t *gnb);
int __wrap_ngap_send_ng_setup_failure(amf_gnb_t *gnb,
        NGAP_Cause_PR group, long cause);

time_t __wrap_time(time_t *out)
{
    if (out) *out = test_clock;
    return test_clock;
}

int __wrap_ngap_send_ng_setup_response(amf_gnb_t *gnb)
{
    setup_result = 1;
    return OGS_OK;
}

int __wrap_ngap_send_ng_setup_failure(amf_gnb_t *gnb,
        NGAP_Cause_PR group, long cause)
{
    setup_result = -1;
    setup_cause = cause;
    return OGS_OK;
}

static amf_gnb_t *make_gnb(ogs_sock_t *sock, uint16_t port)
{
    ogs_sockaddr_t *addr = ogs_calloc(1, sizeof(*addr));
    ogs_assert(addr);
    addr->ogs_sa_family = AF_INET;
    addr->sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr->sin.sin_port = htons(port);
    return amf_gnb_add(sock, addr);
}

static void setup(amf_gnb_t *gnb, const char *name, int copies, int expected)
{
    ogs_ngap_message_t pdu, decoded;
    ogs_pkbuf_t *encoded;
    node_profile_setup_fixture(&pdu, name, name ? strlen(name) : 0,
            copies, 0x4000);
    /* Exercise the actual PrintableString constraints and APER decoder too. */
    encoded = ogs_ngap_encode(&pdu);
    ogs_assert(encoded);
    memset(&decoded, 0, sizeof(decoded));
    ogs_assert(ogs_ngap_decode(&decoded, encoded) == OGS_OK);
    setup_result = 0;
    ngap_handle_ng_setup_request(gnb, &decoded);
    ogs_assert(setup_result == expected);
    ogs_assert(gnb->state.ng_setup_success == (expected == 1));
    ogs_ngap_free(&decoded);
    ogs_pkbuf_free(encoded);
}

static void profile_config(const char *text, int expected, bool enabled)
{
    yaml_parser_t parser;
    yaml_document_t document;
    ogs_assert(yaml_parser_initialize(&parser));
    yaml_parser_set_input_string(&parser, (const unsigned char *)text, strlen(text));
    ogs_assert(yaml_parser_load(&parser, &document));
    ogs_app()->document = &document;
    ogs_app()->config_section_id = 0;
    ogs_assert(amf_context_parse_config() == expected);
    ogs_assert(amf_self()->node_profile_enabled == enabled);
    ogs_app()->document = NULL;
    yaml_document_delete(&document);
    yaml_parser_delete(&parser);
}

int main(void)
{
    ogs_pkbuf_config_t packet_config;
    ogs_sock_t sock;
    ogs_socknode_t node;
    amf_gnb_t *first, *second;
    char initial[57], token[57], tampered[57];
    ogs_plmn_id_t plmn;

    ogs_core_initialize();
    ogs_log_install_domain(&__ogs_sbi_domain, "sbi", OGS_LOG_ERROR);
    ogs_pkbuf_default_init(&packet_config);
    ogs_pkbuf_default_create(&packet_config);
    ogs_app_context_init();
    ogs_app_config_init();
    ogs_global_conf()->max.peer = 1;
    ogs_global_conf()->max.ue = 4;
    ogs_app()->pool.sess = 8;
    ogs_app()->metrics.max_specs = 256;
    ogs_app()->timer_mgr = ogs_timer_mgr_create(32);
    amf_metrics_init();
    amf_context_init();
    ogs_assert(amf_self()->node_profile_enabled);
    memset(&sock, 0, sizeof(sock));
    memset(&node, 0, sizeof(node));
    node.sock = &sock;
    ogs_list_add(&amf_self()->ngap_list, &node);
    first = make_gnb(&sock, 38412);
    second = make_gnb(&sock, 38413);
    ogs_assert(first && second);
    ogs_plmn_id_build(&plmn, 460, 11, 2);
    amf_self()->num_of_plmn_support = 1;
    amf_self()->plmn_support[0].plmn_id = plmn;
    amf_self()->plmn_support[0].num_of_s_nssai = 1;
    amf_self()->plmn_support[0].s_nssai[0].sst = 1;
    amf_self()->plmn_support[0].s_nssai[0].sd.v = 0x010101;
    amf_self()->num_of_served_tai = 1;
    amf_self()->served_tai[0].list2.type = OGS_TAI2_TYPE;
    amf_self()->served_tai[0].list2.num = 1;
    amf_self()->served_tai[0].list2.tai[0].plmn_id = plmn;
    amf_self()->served_tai[0].list2.tai[0].tac.v = 1;

    amf_self()->num_of_served_guami = 1;
    amf_self()->amf_name = "profile-test";
    amf_self()->num_of_integrity_order = 1;
    amf_self()->num_of_ciphering_order = 1;
    amf_self()->time.t3502.value = 12;
    amf_self()->time.t3512.value = 540;
    profile_config("amf: {}\n", OGS_OK, true);
    profile_config("amf:\n  ngap:\n    node_profile:\n      enabled: false\n", OGS_OK, false);
    profile_config("amf:\n  ngap:\n    node_profile:\n      enabled: true\n", OGS_OK, true);
    profile_config("amf:\n  ngap:\n    node_profile: false\n", OGS_ERROR, true);
    profile_config("amf:\n  ngap:\n    node_profile:\n      enabled: wrong\n", OGS_ERROR, true);
    profile_config("amf:\n  ngap:\n    node_profile:\n      enabled: [false]\n", OGS_ERROR, true);
    profile_config("amf: {}\n", OGS_OK, true);

    ogs_assert(gnb_build_ran_node_name(initial, sizeof(initial)) == XCN_AUTH_OK);
    setup(first, initial, 1, 1);
    ogs_assert(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    setup(first, token, 1, 1); /* Repeat setup must work at max.peer == 1. */
    setup(second, initial, 1, -1); /* Replay is global, not per gNB. */
    ogs_assert(setup_cause == NGAP_CauseMisc_unspecified);
    ogs_assert(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    setup(second, token, 1, -1); /* A genuinely new peer is still limited. */
    ogs_assert(setup_cause == NGAP_CauseMisc_control_processing_overload);

    setup(first, initial, 1, -1);
    setup(first, NULL, 0, -1);
    setup(first, "plain-gNB", 1, -1);
    ogs_assert(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    setup(first, token, 2, -1);
    memcpy(tampered, token, sizeof(token));
    tampered[0] = tampered[0] == 'A' ? 'B' : 'A';
    setup(first, tampered, 1, -1);
    setup(first, token, 1, 1); /* Bad/duplicate input did not consume it. */

    test_clock += AMF_NODE_PROFILE_WINDOW + 1;
    setup(first, initial, 1, -1);
    test_clock -= AMF_NODE_PROFILE_WINDOW + 3601;
    ogs_assert(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    setup(first, token, 1, 1); /* Fresh setup after wall-clock rollback. */
    amf_self()->node_profile_enabled = false;
    setup(first, "plain-gNB", 1, 1);
    setup(first, NULL, 0, 1);
    amf_self()->node_profile_enabled = true;
    setup(first, "plain-gNB", 1, -1);
    ogs_assert(gnb_build_ran_node_name(token, sizeof(token)) == XCN_AUTH_OK);
    setup(first, token, 1, 1);

    ogs_list_remove(&amf_self()->ngap_list, &node);
    amf_context_final();
    amf_metrics_final();
    ogs_app_config_final();
    ogs_app_context_final();
    ogs_pkbuf_default_destroy();
    ogs_core_terminate();
    talloc_disable_null_tracking();
    puts("NG Setup: APER, admission, replay, repeat-at-capacity and rollback passed");
    return 0;
}
