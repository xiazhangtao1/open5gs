/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "precedence.h"

int pcf_select_qos_precedence(pcf_sess_t *sess,
        ogs_session_data_t *session_data, int requested)
{
    bool used[256] = { false };
    pcf_app_t *app;
    int i;

    /* SMF encodes its default NAS QoS rule with precedence 255. */
    used[255] = true;
    ogs_list_for_each(&sess->app_list, app) {
        for (i = 0; i < app->num_of_pcc_rule; i++) {
            uint32_t value = app->pcc_rule[i].precedence;
            if (value < OGS_ARRAY_SIZE(used))
                used[value] = true;
        }
    }
    if (session_data) {
        for (i = 0; i < session_data->num_of_pcc_rule; i++) {
            uint32_t value = session_data->pcc_rule[i].precedence;
            if (value < OGS_ARRAY_SIZE(used))
                used[value] = true;
        }
    }

    if (requested >= 0)
        return requested < 255 && !used[requested] ? requested : -1;

    /* Keep the historical preference for 100, but never reuse a live value. */
    for (i = 100; i < 255; i++)
        if (!used[i]) return i;
    for (i = 0; i < 100; i++)
        if (!used[i]) return i;
    return -1;
}
