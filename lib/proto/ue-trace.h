/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef OGS_UE_TRACE_H
#define OGS_UE_TRACE_H
#include <stddef.h>
#include <stdint.h>

#define OGS_TRACE_MAX_PDU 32768
#define OGS_TRACE_WIRE_HEADER 68
enum { OGS_TRACE_NGAP = 1, OGS_TRACE_5GMM, OGS_TRACE_5GSM };
enum { OGS_TRACE_PLAIN = 1, OGS_TRACE_VERIFIED = 2,
    OGS_TRACE_DECRYPTED = 4, OGS_TRACE_MAC_FAILED = 8,
    OGS_TRACE_PREPARED = 16, OGS_TRACE_UNAVAILABLE = 32 };

/* A single NF main thread produces; shutdown must follow that thread's join.
 * begin/commit must be paired without nesting. No packet pointer is retained. */
int ogs_ue_trace_open(const char *nf, const char *boot);
int ogs_ue_trace_active(void);
void ogs_ue_trace_close(void);
void *ogs_ue_trace_begin(unsigned kind, unsigned security, const void *data, size_t len);
void ogs_ue_trace_commit(void *ticket, uint64_t event_seq);
void ogs_ue_trace_record(uint64_t seq, unsigned kind, unsigned security,
        const void *data, size_t len);

/* Reader API also builds as a standalone libc-only shared library for the agent.
 * Exactly one reader per ring (flock), native atomics never accessed by Python. */
void *xcn_trace_reader_open(const char *path);
void xcn_trace_reader_close(void *reader);
int xcn_trace_reader_current(void *reader, const char *path);
void xcn_trace_reader_lease(void *reader, int enabled);
int xcn_trace_reader_read(void *reader, void *out, size_t capacity);
void xcn_trace_reader_stats(void *reader, uint64_t out[5]);
#endif
