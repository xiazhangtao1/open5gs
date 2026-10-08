/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/smf/ngap-build.h"
#include "core/abts.h"

static void ngap_priority_test(abts_case *tc, void *data)
{
    smf_sess_t sess = {0};
    smf_bearer_t bearer = {0};
    OpenAPI_qos_flow_profile_t profile = {0};
    OpenAPI_non_dynamic5_qi_t non_dynamic = {0};
    OpenAPI_arp_t arp = {0};
    OpenAPI_qos_flow_add_modify_request_item_t item_profile = {0};
    const uint8_t priorities[] = {0, 1, 20, 127};
    unsigned int i;

    ogs_log_install_domain(&__smf_log_domain, "smf", OGS_LOG_ERROR);
    bearer.qfi = 2;
    bearer.qos.index = 2;
    bearer.qos.arp.priority_level = 8;
    bearer.qos.arp.pre_emption_capability = OGS_5GC_PRE_EMPTION_DISABLED;
    bearer.qos.arp.pre_emption_vulnerability = OGS_5GC_PRE_EMPTION_ENABLED;
    ogs_list_init(&sess.qos_flow_to_modify_list);
    ogs_list_add(&sess.qos_flow_to_modify_list, &bearer.to_modify_node);
    if (data) {
        sess.h_smf_uri = "http://h-smf.example";
        profile._5qi = bearer.qos.index;
        profile.arp = &arp;
        profile.non_dynamic5_qi = &non_dynamic;
        arp.priority_level = bearer.qos.arp.priority_level;
        arp.preempt_cap = OpenAPI_preemption_capability_NOT_PREEMPT;
        arp.preempt_vuln = OpenAPI_preemption_vulnerability_PREEMPTABLE;
        item_profile.qfi = bearer.qfi;
        item_profile.qos_flow_profile = &profile;
        sess.h_smf_qos_flows_add_mod_request_list = OpenAPI_list_create();
        OpenAPI_list_add(sess.h_smf_qos_flows_add_mod_request_list, &item_profile);
    }
    for (i = 0; i < OGS_ARRAY_SIZE(priorities); i++) {
        NGAP_PDUSessionResourceModifyRequestTransfer_t decoded = {0};
        NGAP_QosFlowAddOrModifyRequestItem_t *item;
        NGAP_NonDynamic5QIDescriptor_t *descriptor;
        ogs_pkbuf_t *encoded;

        bearer.qos.priority_level = priorities[i];
        non_dynamic.is_priority_level = priorities[i] != 0;
        non_dynamic.priority_level = priorities[i];
        encoded = ngap_build_pdu_session_resource_modify_request_transfer(
                &sess, false);
        ogs_assert(encoded);
        ABTS_INT_EQUAL(tc, OGS_OK, ogs_asn_decode(
                &asn_DEF_NGAP_PDUSessionResourceModifyRequestTransfer,
                &decoded, sizeof(decoded), encoded));
        ogs_assert(decoded.protocolIEs.list.count == 1);
        item = decoded.protocolIEs.list.array[0]->value.choice.
            QosFlowAddOrModifyRequestList.list.array[0];
        ogs_assert(item->qosFlowLevelQosParameters);
        descriptor = item->qosFlowLevelQosParameters->qosCharacteristics.
            choice.nonDynamic5QI;
        ogs_assert(descriptor);
        ABTS_INT_EQUAL(tc, 2, item->qosFlowIdentifier);
        ABTS_INT_EQUAL(tc, 2, descriptor->fiveQI);
        ABTS_INT_EQUAL(tc, priorities[i] != 0,
                descriptor->priorityLevelQos != NULL);
        if (descriptor->priorityLevelQos)
            ABTS_INT_EQUAL(tc, priorities[i], *descriptor->priorityLevelQos);
        ABTS_INT_EQUAL(tc, 8, item->qosFlowLevelQosParameters->
                allocationAndRetentionPriority.priorityLevelARP);
        ogs_asn_free(&asn_DEF_NGAP_PDUSessionResourceModifyRequestTransfer,
                &decoded);
        ogs_pkbuf_free(encoded);
    }
    if (data)
        OpenAPI_list_free(sess.h_smf_qos_flows_add_mod_request_list);
}

abts_suite *test_qos_priority(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, ngap_priority_test, NULL);
    abts_run_test(suite, ngap_priority_test, &suite);
    return suite;
}
