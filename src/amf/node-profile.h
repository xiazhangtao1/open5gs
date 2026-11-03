/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef AMF_NODE_PROFILE_H
#define AMF_NODE_PROFILE_H

#include "xcn_auth.h"

#define AMF_NODE_PROFILE_WINDOW 86400U
#define AMF_NODE_PROFILE_CAPACITY 65536U

/* Owned exclusively by the AMF event thread. Initialize before it starts. */
void amf_node_profile_init(void);
int amf_node_profile_check(const char *name, size_t length, uint64_t now);
int amf_node_profile_check_payload(const xcn_auth_payload_t *payload,
        uint64_t now);
size_t amf_node_profile_count(void);

#endif
