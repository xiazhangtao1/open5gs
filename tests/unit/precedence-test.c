/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "../../src/pcf/precedence.h"
#include "core/abts.h"

static void allocation_test(abts_case *tc, void *data)
{
    pcf_sess_t first = {0}, second = {0};
    pcf_app_t a = {0}, b = {0};
    ogs_session_data_t policies = {0};

    ogs_list_init(&first.app_list);
    ogs_list_init(&second.app_list);
    ABTS_INT_EQUAL(tc, 100, pcf_select_qos_precedence(&first, NULL, -1));
    a.num_of_pcc_rule = 1;
    a.pcc_rule[0].precedence = 100;
    ogs_list_add(&first.app_list, &a);
    ABTS_INT_EQUAL(tc, 101, pcf_select_qos_precedence(&first, NULL, -1));
    ABTS_INT_EQUAL(tc, -1, pcf_select_qos_precedence(&first, NULL, 100));
    ABTS_INT_EQUAL(tc, 100, pcf_select_qos_precedence(&second, NULL, -1));
    b.num_of_pcc_rule = 1;
    b.pcc_rule[0].precedence = 101;
    ogs_list_add(&first.app_list, &b);
    ABTS_INT_EQUAL(tc, 102, pcf_select_qos_precedence(&first, NULL, -1));
    ABTS_INT_EQUAL(tc, 100, a.pcc_rule[0].precedence);
    /* Deletion and failed creation both remove the app from this list. */
    ogs_list_remove(&first.app_list, &a);
    ABTS_INT_EQUAL(tc, 100, pcf_select_qos_precedence(&first, NULL, -1));
    policies.num_of_pcc_rule = 1;
    policies.pcc_rule[0].precedence = 100;
    ABTS_INT_EQUAL(tc, 102, pcf_select_qos_precedence(&first, &policies, -1));
    ABTS_INT_EQUAL(tc, -1, pcf_select_qos_precedence(&first, &policies, 100));
    ABTS_INT_EQUAL(tc, -1, pcf_select_qos_precedence(&first, NULL, 255));
    ABTS_INT_EQUAL(tc, -1, pcf_select_qos_precedence(&first, NULL, 256));
    ABTS_INT_EQUAL(tc, 0, pcf_select_qos_precedence(&first, NULL, 0));
    ABTS_INT_EQUAL(tc, 254, pcf_select_qos_precedence(&first, NULL, 254));
}

static void exhaustion_test(abts_case *tc, void *data)
{
    pcf_sess_t sess = {0};
    pcf_app_t *apps = ogs_calloc(255, sizeof(*apps));
    int i;

    ABTS_PTR_NOTNULL(tc, apps);
    ogs_list_init(&sess.app_list);
    for (i = 0; i < 255; i++) {
        apps[i].num_of_pcc_rule = 1;
        apps[i].pcc_rule[0].precedence = i;
        ogs_list_add(&sess.app_list, &apps[i]);
    }
    ABTS_INT_EQUAL(tc, -1, pcf_select_qos_precedence(&sess, NULL, -1));
    ogs_list_remove(&sess.app_list, &apps[42]);
    ABTS_INT_EQUAL(tc, 42, pcf_select_qos_precedence(&sess, NULL, -1));
    ogs_free(apps);
}

abts_suite *test_precedence(abts_suite *suite)
{
    suite = ADD_SUITE(suite);
    abts_run_test(suite, allocation_test, NULL);
    abts_run_test(suite, exhaustion_test, NULL);
    return suite;
}
