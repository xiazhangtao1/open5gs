/*
 * Copyright (C) 2019,2020 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of xcn.
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

#include "test-common.h"

#define MAX_STRESS_UE 1024

static void test_pool_overload(abts_case *tc, ogs_socknode_t *ngap,
        test_ue_t *test_ue)
{
    ogs_nas_5gs_guti_t saved_guti = test_ue->nas_5gs_guti;
    ogs_ngap_message_t message;
    NGAP_UEContextReleaseCommand_t *command;
    NGAP_Cause_t *cause = NULL;
    ogs_pkbuf_t *nas, *packet;
    int i;

    /* A fresh, unknown GUTI needs a new AMF context. At capacity, the
     * AMF must release this connection and continue serving known UEs. */
    memset(&test_ue->nas_5gs_guti, 0, sizeof(test_ue->nas_5gs_guti));
    nas = testgmm_build_registration_request(test_ue, NULL, false, false);
    ABTS_PTR_NOTNULL(tc, nas);
    packet = testngap_build_initial_ue_message(test_ue, nas,
            NGAP_RRCEstablishmentCause_mo_Signalling, false, true);
    ABTS_PTR_NOTNULL(tc, packet);
    ABTS_INT_EQUAL(tc, OGS_OK, testgnb_ngap_send(ngap, packet));

    packet = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, packet);
    ABTS_INT_EQUAL(tc, OGS_OK, ogs_ngap_decode(&message, packet));
    ogs_assert(message.present == NGAP_NGAP_PDU_PR_initiatingMessage);
    ogs_assert(message.choice.initiatingMessage->procedureCode ==
            NGAP_ProcedureCode_id_UEContextRelease);
    command = &message.choice.initiatingMessage->value.choice.UEContextReleaseCommand;
    for (i = 0; i < command->protocolIEs.list.count; i++) {
        NGAP_UEContextReleaseCommand_IEs_t *ie = command->protocolIEs.list.array[i];
        if (ie->id == NGAP_ProtocolIE_ID_id_Cause)
            cause = &ie->value.choice.Cause;
    }
    ogs_assert(cause);
    ABTS_INT_EQUAL(tc, NGAP_Cause_PR_misc, cause->present);
    ABTS_INT_EQUAL(tc, NGAP_CauseMisc_control_processing_overload, cause->choice.misc);
    ogs_ngap_free(&message);
    testngap_recv(test_ue, packet);

    packet = testngap_build_ue_context_release_complete(test_ue);
    ABTS_PTR_NOTNULL(tc, packet);
    ABTS_INT_EQUAL(tc, OGS_OK, testgnb_ngap_send(ngap, packet));
    test_ue->nas_5gs_guti = saved_guti;
    ogs_msleep(100);
}

static void test_cycle(abts_case *tc, int ue_count,
        ogs_nas_5gs_guti_t *previous_guti, int cycle, bool observe)
{
    int rv;
    ogs_socknode_t *ngap;
    ogs_socknode_t *gtpu;
    ogs_pkbuf_t *gmmbuf;
    ogs_pkbuf_t *gsmbuf;
    ogs_pkbuf_t *nasbuf;
    ogs_pkbuf_t *sendbuf;
    ogs_pkbuf_t *recvbuf;
    int i;

    ogs_nas_5gs_mobile_identity_suci_t mobile_identity_suci;
    test_ue_t **test_ue = ogs_calloc(ue_count, sizeof(*test_ue));
    test_sess_t *sess = NULL;
    test_bearer_t *qos_flow = NULL;

    bson_t *doc = NULL;
    ogs_assert(test_ue);

    /* gNB connects to AMF */
    ngap = testngap_client(1, AF_INET);
    ABTS_PTR_NOTNULL(tc, ngap);

    /* gNB connects to UPF */
    gtpu = test_gtpu_server(1, AF_INET);
    ABTS_PTR_NOTNULL(tc, gtpu);

    /* Send NG-Setup Reqeust */
    sendbuf = testngap_build_ng_setup_request(0x4000, 22);
    ABTS_PTR_NOTNULL(tc, sendbuf);
    rv = testgnb_ngap_send(ngap, sendbuf);
    ABTS_INT_EQUAL(tc, OGS_OK, rv);

    /* Receive NG-Setup Response */
    recvbuf = testgnb_ngap_read(ngap);
    ABTS_PTR_NOTNULL(tc, recvbuf);
    ogs_pkbuf_free(recvbuf);

    for (i = 0; i < ue_count; i++) {
        char scheme_output[16];
        snprintf(scheme_output, sizeof(scheme_output), "%010u", (unsigned)i + 1);

        /* Setup Test UE & Session Context */
        memset(&mobile_identity_suci, 0, sizeof(mobile_identity_suci));

        mobile_identity_suci.h.supi_format = OGS_NAS_5GS_SUPI_FORMAT_IMSI;
        mobile_identity_suci.h.type = OGS_NAS_5GS_MOBILE_IDENTITY_SUCI;
        mobile_identity_suci.routing_indicator1 = 0;
        mobile_identity_suci.routing_indicator2 = 0xf;
        mobile_identity_suci.routing_indicator3 = 0xf;
        mobile_identity_suci.routing_indicator4 = 0xf;
        mobile_identity_suci.protection_scheme_id = OGS_PROTECTION_SCHEME_NULL;
        mobile_identity_suci.home_network_pki_value = 0;

        test_ue[i] = test_ue_add_by_suci(
                &mobile_identity_suci, scheme_output);
        ogs_assert(test_ue[i]);
        test_ue[i]->nas_5gs_guti = previous_guti[i];

        test_ue[i]->nr_cgi.cell_id = 0x40001;

        test_ue[i]->nas.registration.tsc = 0;
        test_ue[i]->nas.registration.ksi = OGS_NAS_KSI_NO_KEY_IS_AVAILABLE;
        test_ue[i]->nas.registration.follow_on_request = 1;
        test_ue[i]->nas.registration.value = OGS_NAS_5GS_REGISTRATION_TYPE_INITIAL;

        test_ue[i]->k_string = "465b5ce8b199b49faa5f0a2ee238a6bc";
        test_ue[i]->opc_string = "e8ed289deba952e4283b54e88e6183ca";
    }

    for (i = 0; i < ue_count; i++) {
        if (i > 0)
            test_ue[i]->ran_ue_ngap_id = test_ue[i-1]->ran_ue_ngap_id;
        else
            test_ue[i]->ran_ue_ngap_id = 0;

        /* Send PDU session establishment request */
        sess = test_sess_add_by_dnn_and_psi(test_ue[i], "internet", 5);
        ogs_assert(sess);

        /********** Insert Subscriber in Database */
        doc = test_db_new_simple(test_ue[i]);
        ABTS_PTR_NOTNULL(tc, doc);
        ABTS_INT_EQUAL(tc, OGS_OK, test_db_insert_ue(test_ue[i], doc));

        /* Send Registration request */
        test_ue[i]->registration_request_param.guti = 1;
        gmmbuf = testgmm_build_registration_request(test_ue[i], NULL, false, false);
        ABTS_PTR_NOTNULL(tc, gmmbuf);

        test_ue[i]->registration_request_param.gmm_capability = 1;
        test_ue[i]->registration_request_param.s1_ue_network_capability = 1;
        test_ue[i]->registration_request_param.requested_nssai = 1;
        test_ue[i]->registration_request_param.last_visited_registered_tai = 1;
        test_ue[i]->registration_request_param.ue_usage_setting = 1;
        nasbuf = testgmm_build_registration_request(test_ue[i], NULL, false, false);
        ABTS_PTR_NOTNULL(tc, nasbuf);

        sendbuf = testngap_build_initial_ue_message(test_ue[i], gmmbuf,
                    NGAP_RRCEstablishmentCause_mo_Signalling, false, true);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        if (!previous_guti[i].m_tmsi) {
            /* An unknown GUTI requires identity resolution. */
            recvbuf = testgnb_ngap_read(ngap);
            ABTS_PTR_NOTNULL(tc, recvbuf);
            testngap_recv(test_ue[i], recvbuf);

            gmmbuf = testgmm_build_identity_response(test_ue[i]);
            ABTS_PTR_NOTNULL(tc, gmmbuf);
            sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
            ABTS_PTR_NOTNULL(tc, sendbuf);
            rv = testgnb_ngap_send(ngap, sendbuf);
            ABTS_INT_EQUAL(tc, OGS_OK, rv);
        }

        /* Receive Authentication request */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);

        /* Send Authentication response */
        gmmbuf = testgmm_build_authentication_response(test_ue[i]);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive Security mode command */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);

        /* Send Security mode complete */
        gmmbuf = testgmm_build_security_mode_complete(test_ue[i], nasbuf);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive InitialContextSetupRequest +
         * Registration accept */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_InitialContextSetup,
                test_ue[i]->ngap_procedure_code);

        /* Send UERadioCapabilityInfoIndication */
        sendbuf = testngap_build_ue_radio_capability_info_indication(test_ue[i]);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Send InitialContextSetupResponse */
        sendbuf = testngap_build_initial_context_setup_response(test_ue[i], false);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Send Registration complete */
        gmmbuf = testgmm_build_registration_complete(test_ue[i]);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive Configuration update command */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);

        sess->ul_nas_transport_param.request_type =
            OGS_NAS_5GS_REQUEST_TYPE_INITIAL;
        sess->ul_nas_transport_param.dnn = 1;
        sess->ul_nas_transport_param.s_nssai = 0;

        sess->pdu_session_establishment_param.ssc_mode = 1;
        sess->pdu_session_establishment_param.epco = 1;

        gsmbuf = testgsm_build_pdu_session_establishment_request(sess);
        ABTS_PTR_NOTNULL(tc, gsmbuf);
        gmmbuf = testgmm_build_ul_nas_transport(sess,
                OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, gsmbuf);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive PDUSessionResourceSetupRequest +
         * DL NAS transport +
         * PDU session establishment accept */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_PDUSessionResourceSetup,
                test_ue[i]->ngap_procedure_code);

        /* Send PDUSessionResourceSetupResponse */
        sendbuf = testngap_sess_build_pdu_session_resource_setup_response(sess);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);
    }

    if (observe) {
        fprintf(stderr, "XCN_SOAK_ACTIVE cycle=%d ue=%d\n", cycle, ue_count);
        ogs_msleep(3000);
    }

    for (i = 0; i < ue_count; i++) {
        int target = (i + 1) % ue_count;
        char dst_ip[OGS_ADDRSTRLEN];
        test_sess_t *dst_sess = NULL;

        sess = test_sess_find_by_psi(test_ue[i], 5);
        ogs_assert(sess);
        qos_flow = test_qos_flow_find_by_qfi(sess, 1);
        ogs_assert(qos_flow);

        dst_sess = test_sess_find_by_psi(test_ue[target], 5);
        ogs_assert(dst_sess);
        inet_ntop(AF_INET, &dst_sess->ue_ip.addr,
                dst_ip, sizeof(dst_ip));

        rv = test_gtpu_send_ping(gtpu, qos_flow, dst_ip);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        recvbuf = test_gtpu_read(gtpu);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        ogs_pkbuf_free(recvbuf);
    }

    for (i = 0; i < ue_count; i++) {
        /* Send PDU session establishment request */
        sess = test_sess_find_by_psi(test_ue[i], 5);
        ogs_assert(sess);

        /* Send PDU Session release request */
        sess->ul_nas_transport_param.request_type = 0;
        sess->ul_nas_transport_param.dnn = 0;
        sess->ul_nas_transport_param.s_nssai = 0;

        sess->pdu_session_establishment_param.ssc_mode = 0;
        sess->pdu_session_establishment_param.epco = 0;

        gsmbuf = testgsm_build_pdu_session_release_request(sess);
        ABTS_PTR_NOTNULL(tc, gsmbuf);
        gmmbuf = testgmm_build_ul_nas_transport(sess,
                OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, gsmbuf);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive PDUSessionResourceReleaseCommand +
         * DL NAS transport +
         * PDU session release command */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_PDUSessionResourceRelease,
                test_ue[i]->ngap_procedure_code);

        /* Send PDUSessionResourceReleaseResponse */
        sendbuf = testngap_build_pdu_session_resource_release_response(sess);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Send UplinkNASTransport +
         * UL NAS trasnport +
         * PDU session resource release complete */
        sess->ul_nas_transport_param.request_type = 0;
        sess->ul_nas_transport_param.dnn = 0;
        sess->ul_nas_transport_param.s_nssai = 0;

        sess->pdu_session_establishment_param.ssc_mode = 0;
        sess->pdu_session_establishment_param.epco = 0;

        gsmbuf = testgsm_build_pdu_session_release_complete(sess);
        ABTS_PTR_NOTNULL(tc, gsmbuf);
        gmmbuf = testgmm_build_ul_nas_transport(sess,
                OGS_NAS_PAYLOAD_CONTAINER_N1_SM_INFORMATION, gsmbuf);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);
    }

    for (i = 0; i < ue_count; i++) {
        /* Send De-registration request */
        gmmbuf = testgmm_build_de_registration_request(test_ue[i], 1, true, true);
        ABTS_PTR_NOTNULL(tc, gmmbuf);
        sendbuf = testngap_build_uplink_nas_transport(test_ue[i], gmmbuf);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);

        /* Receive UEContextReleaseCommand */
        recvbuf = testgnb_ngap_read(ngap);
        ABTS_PTR_NOTNULL(tc, recvbuf);
        testngap_recv(test_ue[i], recvbuf);
        ABTS_INT_EQUAL(tc,
                NGAP_ProcedureCode_id_UEContextRelease,
                test_ue[i]->ngap_procedure_code);

        /* Send UEContextReleaseComplete */
        sendbuf = testngap_build_ue_context_release_complete(test_ue[i]);
        ABTS_PTR_NOTNULL(tc, sendbuf);
        rv = testgnb_ngap_send(ngap, sendbuf);
        ABTS_INT_EQUAL(tc, OGS_OK, rv);
    }

    ogs_msleep(300);

    for (i = 0; i < ue_count; i++) {
        /********** Remove Subscriber in Database */
        ABTS_INT_EQUAL(tc, OGS_OK, test_db_remove_ue(test_ue[i]));
    }

    if (observe && ue_count == ogs_global_conf()->max.ue) {
        test_pool_overload(tc, ngap, test_ue[0]);
        fprintf(stderr, "XCN_SOAK_OVERLOAD_RELEASED cycle=%d ue=%d\n", cycle, ue_count);
    }

    /* gNB disonncect from UPF */
    testgnb_gtpu_close(gtpu);

    /* gNB disonncect from AMF */
    testgnb_ngap_close(ngap);

    /* Clear Test UE Context */
    for (i = 0; i < ue_count; i++)
        previous_guti[i] = test_ue[i]->nas_5gs_guti;
    test_ue_remove_all();
    ogs_free(test_ue);

    if (observe) {
        fprintf(stderr, "XCN_SOAK_CLEANUP cycle=%d ue=%d\n", cycle, ue_count);
        ogs_msleep(2000);
    }
}

static unsigned stress_parameter(const char *name, unsigned fallback,
        unsigned minimum, unsigned maximum)
{
    const char *text = getenv(name);
    char *end = NULL;
    unsigned long value;

    if (!text) return fallback;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (text[0] == '-' || end == text || *end || errno == ERANGE ||
            value < minimum || value > maximum) {
        ogs_fatal("%s must be between %u and %u", name, minimum, maximum);
        ogs_assert_if_reached();
    }
    return value;
}

static void test1_func(abts_case *tc, void *data)
{
    static const int stages[] = {16, 64, 128, 256, 512, 1024};
    unsigned capacity = ogs_min(ogs_global_conf()->max.ue, MAX_STRESS_UE);
    unsigned duration = stress_parameter("XCN_STRESS_SECONDS", 0, 0, 86400);
    unsigned fixed = stress_parameter("XCN_STRESS_UE_COUNT", 0, 1, capacity);
    bool observe = duration > 0 || fixed > 0;
    ogs_nas_5gs_guti_t *previous_guti =
        ogs_calloc(capacity, sizeof(*previous_guti));
    ogs_time_t started = ogs_get_monotonic_time();
    unsigned cycle = 0;
    ogs_assert(previous_guti);

    do {
        unsigned stage = ogs_min(cycle, OGS_ARRAY_SIZE(stages) - 1);
        unsigned ue_count = fixed ? fixed :
            (duration ? ogs_min(stages[stage], capacity) : 3);
        ogs_assert(ue_count > 0 && ue_count <= capacity);
        if (observe)
            fprintf(stderr, "XCN_SOAK_START cycle=%u ue=%u elapsed_s=%lld\n",
                    cycle + 1, ue_count,
                    (long long)((ogs_get_monotonic_time() - started) / OGS_USEC_PER_SEC));

        test_cycle(tc, ue_count, previous_guti, ++cycle, observe);

        if (observe)
            fprintf(stderr, "XCN_SOAK_DONE cycle=%u ue=%u elapsed_s=%lld\n",
                    cycle, ue_count,
                    (long long)((ogs_get_monotonic_time() - started) / OGS_USEC_PER_SEC));
    } while (duration && ogs_get_monotonic_time() - started <
            ogs_time_from_sec(duration));

    ogs_free(previous_guti);
}

abts_suite *test_multi_ue(abts_suite *suite)
{
    suite = ADD_SUITE(suite)

    abts_run_test(suite, test1_func, NULL);

    return suite;
}
