#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static uint64_t nsec_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv)
{
    if (argc != 7) {
        fprintf(stderr,
            "usage: %s <src_ip> <dst_ip> <dst_port> <seconds> <mbps|0=max> <payload_bytes>\n",
            argv[0]);
        return 2;
    }

    const char *src_ip = argv[1];
    const char *dst_ip = argv[2];
    int dst_port = atoi(argv[3]);
    int seconds = atoi(argv[4]);
    double mbps = atof(argv[5]);
    int payload_len = atoi(argv[6]);

    if (dst_port <= 0 || dst_port > 65535 ||
        seconds <= 0 || payload_len < 0 || payload_len > 9000)
        return 2;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in src;
    memset(&src, 0, sizeof(src));
    src.sin_family = AF_INET;
    src.sin_port = htons(0);
    if (inet_pton(AF_INET, src_ip, &src.sin_addr) != 1) {
        perror("inet_pton src");
        return 2;
    }
    if (bind(fd, (struct sockaddr *)&src, sizeof(src)) < 0) {
        perror("bind");
        return 1;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons((uint16_t)dst_port);
    if (inet_pton(AF_INET, dst_ip, &dst.sin_addr) != 1) {
        perror("inet_pton dst");
        return 2;
    }

    uint8_t *payload = malloc((size_t)payload_len);
    if (!payload) {
        perror("malloc");
        return 1;
    }
    memset(payload, 0xcd, (size_t)payload_len);

    size_t ip_bytes = 20u + 8u + (size_t)payload_len;
    uint64_t start = nsec_now();
    uint64_t end = start + (uint64_t)seconds * 1000000000ull;
    uint64_t sent = 0, errors = 0;
    uint64_t interval = 0, next = start;

    if (mbps > 0.0) {
        double pps = (mbps * 1000000.0) / ((double)ip_bytes * 8.0);
        if (pps > 0.0)
            interval = (uint64_t)(1000000000.0 / pps);
    }

    while (nsec_now() < end) {
        ssize_t rc = sendto(fd, payload, (size_t)payload_len, 0,
                            (struct sockaddr *)&dst, sizeof(dst));
        if (rc == payload_len)
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
    double ip_mbps =
        ((double)sent * (double)ip_bytes * 8.0) / elapsed / 1000000.0;

    printf("sent_pkts=%lu errors=%lu elapsed=%.3f ip_mbps=%.2f ip_bytes=%zu\n",
        (unsigned long)sent, (unsigned long)errors, elapsed, ip_mbps,
        ip_bytes);

    free(payload);
    return errors ? 1 : 0;
}
