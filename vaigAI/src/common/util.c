/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: utility helpers implementation.
 */
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <inttypes.h>

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_log.h>

/* ── TSC calibration ──────────────────────────────────────────────────────── */
uint64_t g_tsc_hz = 0;

void tgen_calibrate_tsc(void)
{
    struct timespec ts0, ts1;
    uint64_t tsc0, tsc1;
    const uint64_t window_ns = 100000000ULL; /* 100 ms */

    clock_gettime(CLOCK_MONOTONIC, &ts0);
    tsc0 = rte_rdtsc();

    /* Busy-wait for 100 ms */
    do {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
    } while ((uint64_t)(ts1.tv_sec  - ts0.tv_sec)  * 1000000000ULL +
             (uint64_t)(ts1.tv_nsec - ts0.tv_nsec) < window_ns);

    tsc1 = rte_rdtsc();

    uint64_t elapsed_ns =
        (uint64_t)(ts1.tv_sec  - ts0.tv_sec)  * 1000000000ULL +
        (uint64_t)(ts1.tv_nsec - ts0.tv_nsec);
    g_tsc_hz = (tsc1 - tsc0) * 1000000000ULL / elapsed_ns;

    RTE_LOG(INFO, TGEN, "TSC frequency calibrated: %" PRIu64 " Hz (~%" PRIu64 " MHz)\n",
            g_tsc_hz, (uint64_t)(g_tsc_hz / 1000000UL));
}

/* ── IPv4 / MAC formatting ────────────────────────────────────────────────── */
const char *tgen_ipv4_str(uint32_t addr_net, char *buf, size_t len)
{
    struct in_addr ia = { .s_addr = addr_net };
    inet_ntop(AF_INET, &ia, buf, (socklen_t)len);
    return buf;
}

const char *tgen_mac_str(const uint8_t *mac, char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

int tgen_parse_ipv4(const char *str, uint32_t *out_net)
{
    struct in_addr ia;
    if (inet_pton(AF_INET, str, &ia) != 1)
        return -1;
    *out_net = ia.s_addr; /* network byte order */
    return 0;
}

int tgen_parse_cidr(const char *str, uint32_t *out_net, uint8_t *out_len)
{
    char tmp[32];
    char *slash;

    snprintf(tmp, sizeof(tmp), "%s", str);
    slash = strchr(tmp, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    int len = atoi(slash + 1);
    if (len < 0 || len > 32)
        return -1;
    *out_len = (uint8_t)len;
    return tgen_parse_ipv4(tmp, out_net);
}

/* ── Power-of-two ─────────────────────────────────────────────────────────── */
uint64_t tgen_next_pow2_u64(uint64_t v)
{
    if (v == 0) return 1;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;
    return v + 1;
}

/* ── PRNG ─────────────────────────────────────────────────────────────────── */
__thread uint64_t g_prng_state = 0;

void tgen_prng_seed(uint64_t seed)
{
    g_prng_state = seed ? seed : 1; /* xorshift64 must not be 0 */
}

/* ── UUID v4 ──────────────────────────────────────────────────────────────── */
void tgen_uuid_v4(char *buf)
{
    uint64_t a = tgen_rand64();
    uint64_t b = tgen_rand64();
    uint8_t raw[16];
    memcpy(raw,     &a, 8);
    memcpy(raw + 8, &b, 8);

    raw[6] = (raw[6] & 0x0f) | 0x40; /* version 4 */
    raw[8] = (raw[8] & 0x3f) | 0x80; /* variant bits */

    snprintf(buf, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        raw[0],  raw[1],  raw[2],  raw[3],
        raw[4],  raw[5],  raw[6],  raw[7],
        raw[8],  raw[9],  raw[10], raw[11],
        raw[12], raw[13], raw[14], raw[15]);
}
