/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP send buffer for retransmission and window-driven queuing.
 *
 * Linear buffer storing data from snd_una forward:
 *   data[0]                   = byte at snd_una (oldest unACKed)
 *   data[snd_nxt - snd_una]   = first unsent byte
 *   data[len - 1]             = last queued byte
 *
 * On ACK: trim front (memmove).  On drain: send unsent portion.
 * On RTO: retransmit from data[0].
 */
#ifndef TGEN_TCP_SND_BUF_H
#define TGEN_TCP_SND_BUF_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TCP_SND_BUF_DEFAULT_CAP  65536   /* 64 KB — covers 65535 cwnd + headroom */

typedef struct tcp_snd_buf_s {
    uint8_t  *data;
    uint32_t  cap;      /* allocated capacity */
    uint32_t  len;      /* bytes stored (ACKed data already trimmed) */
    uint32_t  base_seq; /* TCP sequence number of data[0] */
} tcp_snd_buf_t;

/** Allocate a send buffer with given capacity.  Returns NULL on failure. */
tcp_snd_buf_t *tcp_snd_buf_alloc(uint32_t cap);

/** Free a send buffer.  Safe to call with NULL. */
void tcp_snd_buf_free(tcp_snd_buf_t *sb);

/** Append data to the buffer.  Returns bytes appended (<= len). */
static inline uint32_t
tcp_snd_buf_append(tcp_snd_buf_t *sb, const uint8_t *data, uint32_t len)
{
    uint32_t space = sb->cap - sb->len;
    if (len > space) len = space;
    if (len == 0) return 0;
    memcpy(sb->data + sb->len, data, len);
    sb->len += len;
    return len;
}

/** Remove acked bytes from the front of the buffer. */
static inline void
tcp_snd_buf_ack(tcp_snd_buf_t *sb, uint32_t acked)
{
    if (acked >= sb->len) {
        sb->base_seq += sb->len;
        sb->len = 0;
        return;
    }
    memmove(sb->data, sb->data + acked, sb->len - acked);
    sb->base_seq += acked;
    sb->len -= acked;
}

/** Return the length of unsent data in the buffer.
 *  in_flight = snd_nxt - snd_una. */
static inline uint32_t
tcp_snd_buf_unsent_len(const tcp_snd_buf_t *sb, uint32_t in_flight)
{
    return (in_flight < sb->len) ? sb->len - in_flight : 0;
}

#ifdef __cplusplus
}
#endif
#endif /* TGEN_TCP_SND_BUF_H */
