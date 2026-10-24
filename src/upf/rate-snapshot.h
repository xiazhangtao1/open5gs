/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef UPF_RATE_SNAPSHOT_H
#define UPF_RATE_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* No session pointers survive the existing sampler's rule read lock. */
typedef struct {
    uint16_t pdr_id;
    uint32_t qer_id, generation;
    uint8_t qfi, direction, src_if, dst_if;
    uint64_t bps, pps, octets, packets;
} upf_snapshot_rule_t;

typedef struct {
    char supi[32], dnn[256];
    uint8_t ipv4[4], ipv6[16], has_ipv4, has_ipv6, worker;
    uint64_t seid, uptime_seconds;
    size_t first_rule, rule_count;
} upf_snapshot_session_t;

typedef struct {
    int state; /* private ownership: free, writing, ready, reading */
    uint64_t version, timestamp_us, monotonic_us, sample_us;
    uint64_t lock_wait_us, lock_hold_us;
    char boot_id[64];
    size_t session_count, rule_count;
    bool truncated;
    upf_snapshot_session_t *sessions;
    upf_snapshot_rule_t *rules;
} upf_snapshot_frame_t;

typedef struct {
    upf_snapshot_frame_t frames[2];
    size_t max_sessions, max_rules;
    uint64_t lease_until_us, version, skipped;
    unsigned next;
    char boot_id[64];
} upf_snapshot_t;

int upf_snapshot_init(upf_snapshot_t *, size_t sessions, size_t rules);
/* Call only after producer and consumer threads have joined. */
void upf_snapshot_final(upf_snapshot_t *);
void upf_snapshot_subscribe(upf_snapshot_t *, uint64_t now_us);
/* Single producer; never waits, allocates, serializes or performs I/O. */
upf_snapshot_frame_t *upf_snapshot_begin(upf_snapshot_t *, uint64_t now_us);
void upf_snapshot_publish(upf_snapshot_t *, upf_snapshot_frame_t *);
/* Single consumer; immutable frame until release. */
upf_snapshot_frame_t *upf_snapshot_acquire(upf_snapshot_t *);
void upf_snapshot_release(upf_snapshot_frame_t *);
size_t upf_snapshot_json(const upf_snapshot_frame_t *, char *, size_t);

#endif
