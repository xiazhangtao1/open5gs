#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint16_t csum16(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += ((uint16_t)p[0] << 8) | p[1];
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (uint16_t)p[0] << 8;

    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);

    return (uint16_t)~sum;
}

static uint64_t nsec_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s <upf_ip> <teid_hex> <inner_src> <inner_dst> <seconds> <mbps|0=max> <inner_payload_bytes>\n",
        argv0);
}

int main(int argc, char **argv)
{
    const size_t gtpu_ext_len = 8;
    uint8_t packet[4096];
    struct sockaddr_in dst;
    uint32_t teid;
    int seconds;
    double mbps;
    int payload_len;
    int fd;

    if (argc != 8) {
        usage(argv[0]);
        return 2;
    }

    teid = (uint32_t)strtoul(argv[2], NULL, 16);
    seconds = atoi(argv[5]);
    mbps = atof(argv[6]);
    payload_len = atoi(argv[7]);
    if (seconds <= 0 || payload_len < 0 || payload_len > 3500) {
        usage(argv[0]);
        return 2;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(2152);
    if (inet_pton(AF_INET, argv[1], &dst.sin_addr) != 1) {
        perror("inet_pton upf_ip");
        return 2;
    }

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }
    if (connect(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        perror("connect");
        return 1;
    }

    uint8_t *p = packet;
    size_t inner_udp_len = 8u + (size_t)payload_len;
    size_t inner_ip_len = 20u + inner_udp_len;
    size_t gtpu_len = 8u + gtpu_ext_len + inner_ip_len;

    if (gtpu_len > sizeof(packet)) {
        fprintf(stderr, "packet too large\n");
        return 2;
    }

    p[0] = 0x34; /* GTPv1 + seq + N-PDU + extension header */
    p[1] = 0xff; /* G-PDU */
    *(uint16_t *)(p + 2) = htons((uint16_t)(gtpu_ext_len + inner_ip_len));
    *(uint32_t *)(p + 4) = htonl(teid);
    p += 8;
    p[0] = 0x00;
    p[1] = 0x00;
    p[2] = 0x00;
    p[3] = 0x85;
    p[4] = 0x01;
    p[5] = 0x10;
    p[6] = 0x01;
    p[7] = 0x00;
    p += gtpu_ext_len;

    struct in_addr inner_src, inner_dst;
    if (inet_pton(AF_INET, argv[3], &inner_src) != 1 ||
        inet_pton(AF_INET, argv[4], &inner_dst) != 1) {
        perror("inet_pton inner");
        return 2;
    }

    p[0] = 0x45;
    p[1] = 0x00;
    *(uint16_t *)(p + 2) = htons((uint16_t)inner_ip_len);
    *(uint16_t *)(p + 4) = htons(0x7000);
    *(uint16_t *)(p + 6) = htons(0x4000);
    p[8] = 64;
    p[9] = 17;
    *(uint16_t *)(p + 10) = 0;
    memcpy(p + 12, &inner_src.s_addr, 4);
    memcpy(p + 16, &inner_dst.s_addr, 4);
    *(uint16_t *)(p + 10) = htons(csum16(p, 20));

    p += 20;
    *(uint16_t *)(p + 0) = htons(40000);
    *(uint16_t *)(p + 2) = htons(9999);
    *(uint16_t *)(p + 4) = htons((uint16_t)inner_udp_len);
    *(uint16_t *)(p + 6) = 0;
    memset(p + 8, 0xab, (size_t)payload_len);

    uint64_t start = nsec_now();
    uint64_t end = start + (uint64_t)seconds * 1000000000ull;
    uint64_t sent = 0, errors = 0;
    uint64_t interval = 0, next = start;

    if (mbps > 0.0) {
        double pps = (mbps * 1000000.0) / ((double)gtpu_len * 8.0);
        if (pps > 0.0)
            interval = (uint64_t)(1000000000.0 / pps);
    }

    while (nsec_now() < end) {
        ssize_t rc = send(fd, packet, gtpu_len, 0);
        if (rc == (ssize_t)gtpu_len)
            sent++;
        else
            errors++;

        if (interval) {
            next += interval;
            uint64_t now = nsec_now();
            if (next > now) {
                struct timespec req = {
                    .tv_sec = (time_t)((next - now) / 1000000000ull),
                    .tv_nsec = (long)((next - now) % 1000000000ull),
                };
                nanosleep(&req, NULL);
            }
        }
    }

    double elapsed = (double)(nsec_now() - start) / 1000000000.0;
    double outer_mbps =
        ((double)sent * (double)gtpu_len * 8.0) / elapsed / 1000000.0;
    double inner_mbps =
        ((double)sent * (double)inner_ip_len * 8.0) / elapsed / 1000000.0;

    printf("sent_pkts=%lu errors=%lu elapsed=%.3f outer_mbps=%.2f inner_ip_mbps=%.2f packet_bytes=%zu inner_ip_bytes=%zu\n",
        (unsigned long)sent, (unsigned long)errors, elapsed, outer_mbps,
        inner_mbps, gtpu_len, inner_ip_len);

    return errors ? 1 : 0;
}
