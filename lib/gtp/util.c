/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
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

#include "ogs-gtp.h"

int ogs_gtpu_parse_header(
        ogs_gtp2_header_desc_t *header_desc, ogs_pkbuf_t *pkbuf)
{
    const uint8_t *data;
    size_t len = OGS_GTPV1U_HEADER_LEN, total;
    uint16_t wire_length;
    uint32_t teid;
    uint8_t next;
    unsigned int count = 0;

    ogs_assert(pkbuf);
    if (header_desc) memset(header_desc, 0, sizeof(*header_desc));
    if (!pkbuf->data || pkbuf->len < OGS_GTPV1U_HEADER_LEN) return -1;
    data = pkbuf->data;
    if ((data[0] & 0xf0) != 0x30) return -1;
    memcpy(&wire_length, data + 2, sizeof(wire_length));
    total = OGS_GTPV1U_HEADER_LEN + be16toh(wire_length);
    if (total != pkbuf->len) return -1;
    if (header_desc) {
        header_desc->flags = data[0];
        header_desc->type = data[1];
        memcpy(&teid, data + 4, sizeof(teid));
        header_desc->teid = be32toh(teid);
    }
    if (!(data[0] & (OGS_GTPU_FLAGS_E|OGS_GTPU_FLAGS_S|OGS_GTPU_FLAGS_PN)))
        return len;
    len += 4;
    if (total < len) return -1;
    if (!(data[0] & OGS_GTPU_FLAGS_E)) return len;
    next = data[len - 1];
    while (next) {
        size_t extension_len;
        uint8_t type = next;
        if (len >= total || ++count > OGS_GTP2_NUM_OF_EXTENSION_HEADER)
            return -1;
        extension_len = (size_t)data[len] * 4;
        if (!extension_len || extension_len > total - len) return -1;
        next = data[len + extension_len - 1];
        if (header_desc) {
            uint16_t value;
            switch (type) {
            case OGS_GTP2_EXTENSION_HEADER_TYPE_PDU_SESSION_CONTAINER:
                header_desc->pdu_type = data[len + 1] >> 4;
                header_desc->qos_flow_identifier = data[len + 2] & 0x3f;
                break;
            case OGS_GTP2_EXTENSION_HEADER_TYPE_UDP_PORT:
                memcpy(&value, data + len + 1, sizeof(value));
                header_desc->udp.presence = true;
                header_desc->udp.port = be16toh(value);
                break;
            case OGS_GTP2_EXTENSION_HEADER_TYPE_PDCP_NUMBER:
                header_desc->pdcp_pdu_presence = true;
                memcpy(&value, data + len + 1, sizeof(value));
                header_desc->pdcp_number_presence = true;
                header_desc->pdcp_number = be16toh(value);
                break;
            case 0x82: /* Long PDCP PDU Number (18-bit sequence number). */
                if (extension_len != 8) return -1;
                header_desc->pdcp_pdu_presence = true;
                break;
            default:
                /* Long PDCP PDU Number and unknown headers remain opaque. */
                break;
            }
        }
        len += extension_len;
    }
    return len;
}

uint16_t ogs_in_cksum(uint16_t *addr, int len)
{
    int nleft = len;
    uint32_t sum = 0;
    uint16_t *w = addr;
    uint16_t answer = 0;

    // Adding 16 bits sequentially in sum
    while (nleft > 1) {
        sum += *w;
        nleft -= 2;
        w++;
    }

    // If an odd byte is left
    if (nleft == 1) {
        *(uint8_t *) (&answer) = *(uint8_t *) w;
        sum += answer;
    }

    sum = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    answer = ~sum;

    return answer;
}

void ogs_gtp2_sender_f_teid(
        ogs_gtp2_sender_f_teid_t *sender_f_teid, ogs_gtp2_message_t *message)
{
    ogs_gtp2_tlv_f_teid_t *tlv_f_teid = NULL;
    ogs_gtp2_f_teid_t *f_teid = NULL;

    ogs_assert(sender_f_teid);
    ogs_assert(message);

    memset(sender_f_teid, 0, sizeof(*sender_f_teid));

    switch (message->h.type) {
    case OGS_GTP2_CREATE_SESSION_REQUEST_TYPE:
        tlv_f_teid = &message->create_session_request.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_CREATE_SESSION_RESPONSE_TYPE:
        tlv_f_teid = &message->create_session_response.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_MODIFY_BEARER_REQUEST_TYPE:
        tlv_f_teid = &message->modify_bearer_request.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_DELETE_SESSION_REQUEST_TYPE:
        tlv_f_teid = &message->delete_session_request.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_MODIFY_BEARER_COMMAND_TYPE:
        tlv_f_teid = &message->modify_bearer_command.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_DELETE_BEARER_COMMAND_TYPE:
        tlv_f_teid = &message->delete_bearer_command.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_BEARER_RESOURCE_COMMAND_TYPE:
        tlv_f_teid = &message->bearer_resource_command.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_CREATE_INDIRECT_DATA_FORWARDING_TUNNEL_REQUEST_TYPE:
        tlv_f_teid = &message->create_indirect_data_forwarding_tunnel_request.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_CREATE_INDIRECT_DATA_FORWARDING_TUNNEL_RESPONSE_TYPE:
        tlv_f_teid = &message->create_indirect_data_forwarding_tunnel_response.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_DOWNLINK_DATA_NOTIFICATION_TYPE:
        tlv_f_teid = &message->downlink_data_notification.
            sender_f_teid_for_control_plane;
        break;
    case OGS_GTP2_MODIFY_ACCESS_BEARERS_REQUEST_TYPE:
        tlv_f_teid = &message->modify_access_bearers_request.
            sender_f_teid_for_control_plane;
    default:
        break;
    }

    if (tlv_f_teid && tlv_f_teid->presence && (f_teid = tlv_f_teid->data)) {
        sender_f_teid->teid_presence = true;
        sender_f_teid->teid = be32toh(f_teid->teid);
    }
}
