/* SPDX-License-Identifier: BSD-3-Clause */
#include "histogram.h"

uint64_t
hist_percentile(const histogram_t *h, double p)
{
    if (h->total_count == 0) return 0;

    uint64_t target = (uint64_t)((p / 100.0) * (double)h->total_count);
    if (target == 0) target = 1;

    uint64_t seen = 0;
    for (uint32_t i = 0; i < HIST_BUCKETS; i++) {
        seen += h->counts[i];
        if (seen >= target) {
            /* Upper bound of bucket i is 2^(i+1) µs */
            return (1ull << (i + 1));
        }
    }
    return h->max_us;
}
