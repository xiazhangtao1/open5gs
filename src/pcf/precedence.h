/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef PCF_PRECEDENCE_H
#define PCF_PRECEDENCE_H

#include "context.h"

/* -1 requests automatic allocation; -1 returned means no available value. */
int pcf_select_qos_precedence(pcf_sess_t *sess,
        ogs_session_data_t *session_data, int requested);

#endif
