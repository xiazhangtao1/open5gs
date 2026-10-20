/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef XCNCTL_INSPECT_H
#define XCNCTL_INSPECT_H

#include <stdbool.h>
#include <stdint.h>
#include "sbi/openapi/external/cJSON.h"

typedef struct {
    const char *supi;
    const char *plmn;
    bool has_gnb_id;
    uint32_t gnb_id;
    bool detail;
    bool json;
    const char *ue_url;
    const char *gnb_url;
    const char *smf_url;
} xcnctl_inspect_options_t;

/* Complete, owned {items: []} result, or NULL on any invalid/partial page. */
cJSON *xcnctl_load_info(const char *url);
int xcnctl_inspect(const char *kind, const xcnctl_inspect_options_t *options);
char *xcnctl_add_rate_access(const char *response, const cJSON *ues,
        bool json, bool with_location);

#endif
