#ifndef XCN_AUTH_H
#define XCN_AUTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCN_AUTH_RAN_NAME_LEN 56
#define XCN_AUTH_RANDOM_LEN 8

typedef struct {
    uint64_t timestamp;
    uint8_t random[XCN_AUTH_RANDOM_LEN];
} xcn_auth_payload_t;

enum {
    XCN_AUTH_OK = 0,
    XCN_AUTH_ERR_PARAM = -1,
    XCN_AUTH_ERR_RANDOM = -2,
    XCN_AUTH_ERR_FORMAT = -3,
    XCN_AUTH_ERR_BASE64 = -4,
    XCN_AUTH_ERR_CRYPTO = -5,
    XCN_AUTH_ERR_TAG = -6,
    XCN_AUTH_ERR_TIME = -7,
    XCN_AUTH_ERR_REPLAY = -8
};

/* Requires at least XCN_AUTH_RAN_NAME_LEN + 1 bytes, including the NUL.
 * On failure, a nonempty output buffer is set to an empty string. */
int gnb_build_ran_node_name(char *output, size_t output_size);

/* Input is length-delimited; a NUL terminator is not required.
 * Verifies encoding and the GCM tag only. The caller checks time/replay.
 * On failure, a non-NULL payload is cleared. No key is exposed by this API. */
int amf_verify_ran_node_name(const char *name, size_t name_len,
        xcn_auth_payload_t *payload);

#ifdef __cplusplus
}
#endif
#endif
