/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: utility helpers shared across modules.
 */
#ifndef TGEN_UTIL_H
#define TGEN_UTIL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── TSC / timing ─────────────────────────────────────────────────────────── */
/** TSC frequency in Hz — calibrated once at startup. */
extern uint64_t g_tsc_hz;

/** Calibrate TSC frequency over a 100 ms window vs. CLOCK_MONOTONIC. */
void tgen_calibrate_tsc(void);

/** Convert a TSC delta to microseconds. */
static inline uint64_t tgen_tsc_to_us(uint64_t delta)
{
    return delta * 1000000ULL / (g_tsc_hz ? g_tsc_hz : 1000000000ULL);
}

/** Convert a TSC delta to nanoseconds. */
static inline uint64_t tgen_tsc_to_ns(uint64_t delta)
{
    return delta * 1000000000ULL / (g_tsc_hz ? g_tsc_hz : 1000000000ULL);
}

/* ── String / formatting helpers ─────────────────────────────────────────── */
/** Format an IPv4 address to a caller-supplied buffer (at least 16 bytes). */
const char *tgen_ipv4_str(uint32_t addr, char *buf, size_t len);

/** Format a MAC address to a caller-supplied buffer (at least 18 bytes). */
const char *tgen_mac_str(const uint8_t *mac, char *buf, size_t len);

/** Parse a dotted-decimal IPv4 address into network-byte-order uint32_t.
 *  Returns 0 on success, -1 on parse error. */
int tgen_parse_ipv4(const char *str, uint32_t *out_net);

/** Parse an IPv4 CIDR prefix ("a.b.c.d/N").
 *  Returns 0 on success, -1 on parse error. */
int tgen_parse_cidr(const char *str, uint32_t *out_net, uint8_t *out_len);

/** Return the next power of two >= v (64-bit). */
uint64_t tgen_next_pow2_u64(uint64_t v);

/** Return true if v is a power of two. */
static inline bool tgen_is_pow2(uint64_t v)
{
    return v != 0 && (v & (v - 1)) == 0;
}

/* ── Pseudo-random (fast, non-cryptographic) ──────────────────────────────── */
/** Per-core xorshift64 PRNG seed — initialised from TSC + lcore_id. */
extern __thread uint64_t g_prng_state;

/** Seed the PRNG for the calling thread. */
void tgen_prng_seed(uint64_t seed);

/** Return a uniformly random 64-bit value (xorshift64). */
static inline uint64_t tgen_rand64(void)
{
    uint64_t x = g_prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return (g_prng_state = x);
}

/** Return a uniformly random integer in [lo, hi]. */
static inline uint64_t tgen_rand_range(uint64_t lo, uint64_t hi)
{
    if (hi <= lo) return lo;
    return lo + (tgen_rand64() % (hi - lo + 1));
}

/** Generate a UUID v4 string into buf (must be >= 37 bytes). */
void tgen_uuid_v4(char *buf);

#ifdef __cplusplus
}
#endif
#endif /* TGEN_UTIL_H */
