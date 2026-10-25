/*
 * Copyright (C) 2019,2020 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
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

void testgsm_handle_pdu_session_establishment_accept(test_sess_t *sess,
        ogs_nas_5gs_pdu_session_establishment_accept_t
            *pdu_session_establishment_accept)
{
    ogs_nas_pdu_address_t *pdu_address = NULL;

    ogs_assert(sess);
    ogs_assert(pdu_session_establishment_accept);

    pdu_address = &pdu_session_establishment_accept->pdu_address;

    if (pdu_session_establishment_accept->presencemask &
            OGS_NAS_5GS_PDU_SESSION_ESTABLISHMENT_ACCEPT_PDU_ADDRESS_PRESENT) {
        switch (pdu_address->pdn_type) {
        case OGS_PDU_SESSION_TYPE_IPV4:
            sess->ue_ip.ipv4 = 1;
            sess->ue_ip.addr = pdu_address->addr;
            break;
        case OGS_PDU_SESSION_TYPE_IPV6:
            sess->ue_ip.ipv6 = 1;
            memset(sess->ue_ip.addr6, 0, 8);
            memcpy(sess->ue_ip.addr6+8, pdu_address->addr6, 8);
            break;
        case OGS_PDU_SESSION_TYPE_IPV4V6:
            sess->ue_ip.ipv4 = 1;
            sess->ue_ip.addr = pdu_address->both.addr;
            sess->ue_ip.ipv6 = 1;
            memset(sess->ue_ip.addr6, 0, 8);
            memcpy(sess->ue_ip.addr6+8, pdu_address->both.addr6, 8);
            break;
        default:
            ogs_fatal("Invalid PDU Address Type [%d]", pdu_address->pdn_type);
            ogs_assert_if_reached();
        }

    }
}

void testgsm_handle_pdu_session_modification_command(test_sess_t *sess,
        ogs_nas_5gs_pdu_session_modification_command_t
            *pdu_session_modification_command)
{
    ogs_assert(sess);
    ogs_assert(pdu_session_modification_command);
    memset(&sess->modification, 0, sizeof(sess->modification));
    sess->modification.ambr_present = !!(pdu_session_modification_command->presencemask &
            OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND_SESSION_AMBR_PRESENT);
    sess->modification.rules_present = !!(pdu_session_modification_command->presencemask &
            OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND_AUTHORIZED_QOS_RULES_PRESENT);
    if (sess->modification.ambr_present) {
        sess->modification.ambr.uplink = ogs_nas_bitrate_to_uint64(&pdu_session_modification_command->session_ambr.uplink);
        sess->modification.ambr.downlink = ogs_nas_bitrate_to_uint64(&pdu_session_modification_command->session_ambr.downlink);
    }
    if (pdu_session_modification_command->presencemask &
            OGS_NAS_5GS_PDU_SESSION_MODIFICATION_COMMAND_AUTHORIZED_QOS_FLOW_DESCRIPTIONS_PRESENT) {
        ogs_nas_qos_flow_description_t flows[OGS_NAS_MAX_NUM_OF_QOS_FLOW_DESCRIPTION];
        int i;
        memset(flows, 0, sizeof(flows));
        sess->modification.flow_count = ogs_nas_parse_qos_flow_descriptions(flows,
                &pdu_session_modification_command->authorized_qos_flow_descriptions);
        ogs_assert(sess->modification.flow_count > 0);
        sess->modification.qfi = flows[0].identifier;
        ogs_assert(flows[0].code == OGS_NAS_MODIFY_NEW_QOS_FLOW_DESCRIPTION ||
                   flows[0].code == OGS_NAS_CREATE_NEW_QOS_FLOW_DESCRIPTION ||
                   flows[0].code == OGS_NAS_DELETE_NEW_QOS_FLOW_DESCRIPTION);
        for (i = 0; i < flows[0].num_of_parameter; i++)
            if (flows[0].param[i].identifier == OGS_NAX_QOS_FLOW_PARAMETER_ID_5QI)
                sess->modification.five_qi = flows[0].param[i].qos_index;
    }
}
