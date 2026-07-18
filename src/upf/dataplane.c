/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "context.h"
#include "dataplane.h"
#include "n3-memif.h"
#include "n6-memif.h"

static ogs_thread_t *worker;
static ogs_thread_mutex_t rule_mutex;
static int stopping;

static bool is_stopping(void)
{
    return __atomic_load_n(&stopping, __ATOMIC_ACQUIRE);
}

static void dataplane_main(void *data)
{
    int n3_rv;
    int n6_rv;

    ogs_info("UPF memif dataplane worker started");
    while (!is_stopping()) {
        upf_dataplane_lock();
        n3_rv = upf_n3_memif_poll();
        n6_rv = upf_n6_memif_poll();
        upf_dataplane_unlock();

        if (n3_rv == OGS_DONE && n6_rv == OGS_DONE)
            ogs_usleep(1000);
    }
    ogs_info("UPF memif dataplane worker stopped");
}

void upf_dataplane_init(void)
{
    ogs_thread_mutex_init(&rule_mutex);
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
}

void upf_dataplane_final(void)
{
    ogs_assert(!worker);
    ogs_thread_mutex_destroy(&rule_mutex);
}

int upf_dataplane_start(void)
{
    ogs_assert(!worker);

    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);

    worker = ogs_thread_create(dataplane_main, NULL);
    return worker ? OGS_OK : OGS_ERROR;
}

void upf_dataplane_stop(void)
{
    if (!worker)
        return;

    __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);

    upf_n3_memif_cancel_poll();
    upf_n6_memif_cancel_poll();
    ogs_thread_destroy(worker);
    worker = NULL;
}

void upf_dataplane_lock(void)
{
    ogs_thread_mutex_lock(&rule_mutex);
}

void upf_dataplane_unlock(void)
{
    ogs_thread_mutex_unlock(&rule_mutex);
}
