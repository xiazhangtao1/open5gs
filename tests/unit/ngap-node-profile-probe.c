/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "node-profile-fixture.h"
#include "xcn_auth.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/sctp.h>
#include <sys/socket.h>
#include <unistd.h>

static int exchange(int fd, const ogs_pkbuf_t *packet)
{
    unsigned char data[65536];
    size_t used = 0;
    struct sctp_sndrcvinfo info;
    ogs_ngap_message_t response;
    ogs_pkbuf_t *received;
    int flags = 0, result = 0;
    ssize_t length;

    length = sctp_sendmsg(fd, packet->data, packet->len, NULL, 0,
            htonl(60), 0, 0, 0, 0);
    if (length != packet->len) return 0;
    do {
        length = sctp_recvmsg(fd, data + used, sizeof(data) - used,
                NULL, NULL, &info, &flags);
        if (length <= 0 || (flags & MSG_NOTIFICATION)) return 0;
        used += length;
        if (used == sizeof(data) && !(flags & MSG_EOR)) return 0;
    } while (!(flags & MSG_EOR));
    received = ogs_pkbuf_alloc(NULL, used);
    if (!received) return 0;
    ogs_pkbuf_put_data(received, data, used);
    memset(&response, 0, sizeof(response));
    if (ogs_ngap_decode(&response, received) == OGS_OK) {
        if (response.present == NGAP_NGAP_PDU_PR_successfulOutcome &&
                response.choice.successfulOutcome->procedureCode == NGAP_ProcedureCode_id_NGSetup)
            result = 1;
        else if (response.present == NGAP_NGAP_PDU_PR_unsuccessfulOutcome &&
                response.choice.unsuccessfulOutcome->procedureCode == NGAP_ProcedureCode_id_NGSetup)
            result = -1;
    }
    ogs_ngap_free(&response);
    ogs_pkbuf_free(received);
    return result;
}

int main(int argc, char **argv)
{
    struct sockaddr_in address;
    struct timeval timeout = {5, 0};
    struct sctp_initmsg init;
    ogs_pkbuf_config_t config;
    ogs_ngap_message_t request;
    ogs_pkbuf_t *packet;
    char name[XCN_AUTH_RAN_NAME_LEN + 1];
    const char *mode;
    size_t length;
    int fd, copies = 1, expected = 1, count = 1, i, result = 1;
    unsigned long port;
    char *end;

    if (argc < 3 || argc > 4) {
        fprintf(stderr, "usage: ngap-node-profile-probe IPv4 MODE [PORT]\n"
                "MODE: valid, repeat, replay, plain, missing, duplicate, tamper, stdin, stdin-fail\n");
        return 2;
    }
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    if (inet_pton(AF_INET, argv[1], &address.sin_addr) != 1) return 2;
    port = argc == 4 ? strtoul(argv[3], &end, 10) : 38412;
    if ((argc == 4 && (!*argv[3] || *end)) || !port || port > 65535) return 2;
    address.sin_port = htons((uint16_t)port);
    mode = argv[2];
    if (strcmp(mode, "valid") && strcmp(mode, "repeat") && strcmp(mode, "replay") &&
            strcmp(mode, "plain") && strcmp(mode, "missing") && strcmp(mode, "duplicate") &&
            strcmp(mode, "tamper") && strcmp(mode, "stdin") && strcmp(mode, "stdin-fail"))
        return 2;
    if (gnb_build_ran_node_name(name, sizeof(name)) != XCN_AUTH_OK) return 1;
    length = XCN_AUTH_RAN_NAME_LEN;
    if (!strcmp(mode, "plain")) {
        strcpy(name, "profile-probe"); length = strlen(name); expected = -1;
    } else if (!strcmp(mode, "missing")) {
        copies = 0; expected = -1;
    } else if (!strcmp(mode, "duplicate")) {
        copies = 2; expected = -1;
    } else if (!strcmp(mode, "tamper")) {
        name[0] = name[0] == 'A' ? 'B' : 'A'; expected = -1;
    } else if (!strncmp(mode, "stdin", 5)) {
        if (fread(name, 1, 56, stdin) != 56) return 2;
        if (!strcmp(mode, "stdin-fail")) expected = -1;
    } else if (!strcmp(mode, "repeat")) count = 5;

    ogs_core_initialize();
    ogs_log_install_domain(&__ogs_ngap_domain, "ngap", OGS_LOG_ERROR);
    ogs_pkbuf_default_init(&config);
    ogs_pkbuf_default_create(&config);
    fd = socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP);
    if (fd < 0) { perror("SCTP socket"); return 1; }
    memset(&init, 0, sizeof(init));
    init.sinit_num_ostreams = 4;
    init.sinit_max_instreams = 4;
    if (setsockopt(fd, IPPROTO_SCTP, SCTP_INITMSG, &init, sizeof(init)) ||
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) ||
            connect(fd, (struct sockaddr *)&address, sizeof(address))) {
        perror("SCTP connect"); close(fd); return 1;
    }
    for (i = 0; i < count; i++) {
        if (i && gnb_build_ran_node_name(name, sizeof(name)) != XCN_AUTH_OK) break;
        node_profile_setup_fixture(&request, name, length, copies, 0x3e0001);
        packet = ogs_ngap_encode(&request);
        if (!packet) break;
        result = exchange(fd, packet);
        printf("%s %d: %s\n", mode, i + 1,
                result == 1 ? "NGSetupResponse" : result == -1 ? "NGSetupFailure" : "transport/decode error");
        if (result == expected && !strcmp(mode, "replay")) {
            result = exchange(fd, packet);
            printf("replay 2: %s\n", result == -1 ? "NGSetupFailure" : "unexpected response");
            expected = -1;
        }
        ogs_pkbuf_free(packet);
        if (result != expected) break;
    }
    close(fd);
    ogs_pkbuf_default_destroy();
    ogs_core_terminate();
    talloc_disable_null_tracking();
    return i == count && result == expected ? 0 : 1;
}
