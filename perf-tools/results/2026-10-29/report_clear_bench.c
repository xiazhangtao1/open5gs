/* Diagnostic microbenchmark, not a UPF throughput test.
 * cc -O2 -Wall -Wextra -o /tmp/report-clear report_clear_bench.c
 * taskset -c 34 /tmp/report-clear
 * Both implementations fully clear the same aligned, 712-byte object.
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { REPORT_BYTES = 712, ITERATIONS = 10000000 };
typedef void (*clear_fn)(void *);
static void *(*volatile libc_memset)(void *, int, size_t) = memset;

__attribute__((noinline)) static void clear_rep(void *buffer)
{
    void *position = buffer;
    size_t words = REPORT_BYTES / sizeof(uint64_t);
    _Static_assert(REPORT_BYTES % sizeof(uint64_t) == 0, "whole words required");
#if defined(__x86_64__)
    __asm__ volatile("rep stosq"
                     : "+D"(position), "+c"(words) : "a"(UINT64_C(0)) : "memory");
#else
#error This benchmark compares the x86_64 REP STOSQ observed in the UPF binary.
#endif
}

__attribute__((noinline)) static void clear_libc(void *buffer)
{
    libc_memset(buffer, 0, REPORT_BYTES);
}

static uint64_t now_ns(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &value))
        return 0;
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) + value.tv_nsec;
}

int main(void)
{
    _Alignas(64) unsigned char buffer[REPORT_BYTES];
    clear_fn volatile functions[2] = {clear_rep, clear_libc};
    const char *names[2] = {"rep_stosq", "libc_memset"};
    /* SysV requires DF clear at C call boundaries; set it outside timing. */
    __asm__ volatile("cld" : : : "cc");
    puts("round,implementation,iterations,bytes,ns_per_call");
    for (unsigned round = 0; round < 5; ++round) {
        for (unsigned order = 0; order < 2; ++order) {
            unsigned mode = (round + order) % 2;
            memset(buffer, 0xa5, sizeof(buffer));
            uint64_t started = now_ns();
            for (unsigned i = 0; i < ITERATIONS; ++i) {
                functions[mode](buffer);
                __asm__ volatile("" : : "r"(buffer) : "memory");
            }
            uint64_t ended = now_ns();
            if (!started || ended <= started)
                return 1;
            for (size_t i = 0; i < sizeof(buffer); ++i)
                if (buffer[i] != 0)
                    return 2;
            printf("%u,%s,%u,%u,%.3f\n", round, names[mode], ITERATIONS,
                   REPORT_BYTES, (double)(ended - started) / ITERATIONS);
        }
    }
    return 0;
}
