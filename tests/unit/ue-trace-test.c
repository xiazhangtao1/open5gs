/* SPDX-License-Identifier: AGPL-3.0-or-later */
#define _GNU_SOURCE
#include "../../lib/proto/ue-trace.h"
#include <assert.h>
#include <endian.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *wrong_thread(void *unused)
{
    (void)unused;
    ogs_ue_trace_record(42, OGS_TRACE_NGAP, 0, "bad", 3);
    return NULL;
}

int main(void)
{
    char dir[] = "/tmp/xcn-trace-test-XXXXXX", path[256];
    const char *boot = "00000000-0000-0000-0000-000000000001";
    uint8_t out[OGS_TRACE_MAX_PDU + OGS_TRACE_WIRE_HEADER];
    uint64_t stats[5], seq;
    assert(mkdtemp(dir));
    setenv("XCN_TRACE_DIR", dir, 1);
    setenv("XCN_TRACE_ENABLED", "1", 1);
    assert(ogs_ue_trace_open("AMF", boot));
    snprintf(path, sizeof(path), "%s/AMF.ring", dir);
    void *reader = xcn_trace_reader_open(path);
    assert(reader && !xcn_trace_reader_open(path));
    ogs_ue_trace_record(1, OGS_TRACE_NGAP, 0, "old", 3);
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 0);
    xcn_trace_reader_lease(reader, 1);
    ogs_ue_trace_record(UINT64_MAX, OGS_TRACE_5GMM, OGS_TRACE_VERIFIED, "abc", 3);
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 71);
    memcpy(&seq, out + 8, 8);
    assert(be64toh(seq) == UINT64_MAX && out[4] == OGS_TRACE_5GMM && out[5] == OGS_TRACE_VERIFIED);
    assert(!memcmp(out, "XTR1", 4) && !memcmp(out + 28, boot, 36) && !memcmp(out + 68, "abc", 3));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wrong_thread, NULL));
    assert(!pthread_join(thread, NULL));
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 0);
    /* Capacity exhaustion never overwrites an unread slot. */
    for (unsigned i = 0; i < 257; ++i) ogs_ue_trace_record(i + 1, OGS_TRACE_NGAP, 0, &i, sizeof(i));
    for (unsigned i = 0; i < 256; ++i) {
        assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 72);
        memcpy(&seq, out + 8, 8); assert(be64toh(seq) == i + 1);
        assert(!memcmp(out + 68, &i, sizeof(i)));
    }
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 0);
    /* Oversize is checked before touching the supplied pointer. */
    ogs_ue_trace_record(900, OGS_TRACE_NGAP, 0, "x", OGS_TRACE_MAX_PDU + 1);
    void *ticket = ogs_ue_trace_begin(OGS_TRACE_NGAP, 0, "x", 1);
    assert(ticket);
    ogs_ue_trace_commit(ticket, 0);
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 0);
    xcn_trace_reader_stats(reader, stats);
    assert(stats[0] == 257 && stats[1] == 1 && stats[2] == 1 && stats[4] == 1);
    xcn_trace_reader_lease(reader, 0);
    ogs_ue_trace_record(1000, OGS_TRACE_NGAP, 0, "x", 1);
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 0);
    assert(ogs_ue_trace_open("AMF", "00000000-0000-0000-0000-000000000002"));
    assert(!xcn_trace_reader_current(reader, path));
    xcn_trace_reader_close(reader);
    reader = xcn_trace_reader_open(path); assert(reader);
    xcn_trace_reader_lease(reader, 1);
    ogs_ue_trace_record(1, OGS_TRACE_NGAP, 0, "new", 3);
    assert(xcn_trace_reader_read(reader, out, sizeof(out)) == 71 && out[63] == '2');
    xcn_trace_reader_close(reader); ogs_ue_trace_close();
    unlink(path); rmdir(dir);
    puts("trace lease, identity, ownership, bounds, capacity and restart: PASS");
    return 0;
}
