/*
 * Copyright (C) 2026 by Open5GS Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "context.h"
#include "dataplane.h"
#include "n3-memif.h"
#include "n6-memif.h"

static ogs_thread_t *worker;
static struct {
    unsigned int next;
    unsigned int owner;
} rule_lock;
static int stopping;

static void cpu_relax(void)
{
#if defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#else
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
#endif
}

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
    __atomic_store_n(&rule_lock.next, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&rule_lock.owner, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
}

void upf_dataplane_final(void)
{
    ogs_assert(!worker);
    ogs_assert(__atomic_load_n(&rule_lock.next, __ATOMIC_RELAXED) ==
            __atomic_load_n(&rule_lock.owner, __ATOMIC_RELAXED));
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
    unsigned int ticket =
        __atomic_fetch_add(&rule_lock.next, 1, __ATOMIC_RELAXED);

    while (__atomic_load_n(&rule_lock.owner, __ATOMIC_ACQUIRE) != ticket)
        cpu_relax();
}

void upf_dataplane_unlock(void)
{
    unsigned int owner =
        __atomic_load_n(&rule_lock.owner, __ATOMIC_RELAXED);

    __atomic_store_n(&rule_lock.owner, owner + 1, __ATOMIC_RELEASE);
}
