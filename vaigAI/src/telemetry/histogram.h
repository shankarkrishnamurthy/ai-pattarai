/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: Latency histogram (§6.3) — HDR-style, power-of-2 buckets.
 *
 * Resolution: up to 64 buckets covering [1 µs, 1 s] in powers of 2.
 * Thread-safety: single writer (one worker), single reader (mgmt).
 * No atomics needed — snapshot copies are taken by management thread.
 */
#ifndef TGEN_HISTOGRAM_H
#define TGEN_HISTOGRAM_H

#include <stdint.h>
#include <rte_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIST_BUCKETS 64u  /* log2 buckets: index = clz(value) complement */

typedef struct {
    uint64_t counts[HIST_BUCKETS];
    uint64_t total_count;
    uint64_t total_sum_us;
    uint64_t min_us;
    uint64_t max_us;
} __rte_cache_aligned histogram_t;

/** Reset histogram to zero. */
static inline void
hist_reset(histogram_t *h)
{
    __builtin_memset(h, 0, sizeof(*h));
    h->min_us = UINT64_MAX;
}

/**
 * Record one latency sample (in microseconds).
 * Bucket index = 63 - __builtin_clzll(us | 1).
 */
static inline void
hist_record(histogram_t *h, uint64_t us)
{
    uint32_t idx = 63u - (uint32_t)__builtin_clzll(us | 1ull);
    if (idx >= HIST_BUCKETS) idx = HIST_BUCKETS - 1;
    h->counts[idx]++;
    h->total_count++;
    h->total_sum_us += us;
    if (us < h->min_us) h->min_us = us;
    if (us > h->max_us) h->max_us = us;
}

/**
 * Return the approximate p-th percentile (0–100) in microseconds.
 * Returns 0 if histogram is empty.
 */
uint64_t hist_percentile(const histogram_t *h, double p);

/** Copy src → dst (used by management thread to take a snapshot). */
static inline void
hist_copy(histogram_t *dst, const histogram_t *src)
{
    __builtin_memcpy(dst, src, sizeof(*dst));
}

#ifdef __cplusplus
}
#endif
#endif /* TGEN_HISTOGRAM_H */
