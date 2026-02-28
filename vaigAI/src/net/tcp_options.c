/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP option parsing and generation.
 */
#include "tcp_options.h"
#include <string.h>
#include <rte_byteorder.h>

/* ── Parse ────────────────────────────────────────────────────────────────── */
int tcp_options_parse(const struct rte_tcp_hdr *tcp, tcp_parsed_opts_t *out)
{
    memset(out, 0, sizeof(*out));

    uint8_t doff = (tcp->data_off >> 4) & 0x0F;
    if (doff < 5) return -1;
    size_t opts_len = (size_t)(doff - 5) * 4;

    const uint8_t *p   = (const uint8_t *)tcp + sizeof(*tcp);
    const uint8_t *end = p + opts_len;

    while (p < end) {
        uint8_t kind = *p++;
        if (kind == TCPOPT_EOL) break;
        if (kind == TCPOPT_NOP) continue;
        if (p >= end) break;
        uint8_t len = *p++;
        if (len < 2 || p + len - 2 > end) break;
        const uint8_t *v = p;
        p += len - 2;

        switch (kind) {
        case TCPOPT_MSS:
            if (len == 4) {
                out->mss     = (uint16_t)((v[0] << 8) | v[1]);
                out->has_mss = true;
            }
            break;
        case TCPOPT_WINDOW_SCALE:
            if (len == 3) {
                out->wscale     = v[0];
                out->has_wscale = true;
            }
            break;
        case TCPOPT_SACK_PERM:
            out->has_sack_perm = true;
            break;
        case TCPOPT_TIMESTAMP:
            if (len == 10) {
                out->ts_val          = ((uint32_t)v[0]<<24)|((uint32_t)v[1]<<16)|
                                       ((uint32_t)v[2]<<8)|v[3];
                out->ts_ecr          = ((uint32_t)v[4]<<24)|((uint32_t)v[5]<<16)|
                                       ((uint32_t)v[6]<<8)|v[7];
                out->has_timestamps  = true;
            }
            break;
        case TCPOPT_SACK:
            if (len >= 10 && out->sack_count < 4) {
                for (uint8_t i = 0; i < (len-2)/8 && out->sack_count < 4; i++) {
                    const uint8_t *sb = v + i*8;
                    out->sack[out->sack_count].left  =
                        ((uint32_t)sb[0]<<24)|((uint32_t)sb[1]<<16)|
                        ((uint32_t)sb[2]<<8)|sb[3];
                    out->sack[out->sack_count].right =
                        ((uint32_t)sb[4]<<24)|((uint32_t)sb[5]<<16)|
                        ((uint32_t)sb[6]<<8)|sb[7];
                    out->sack_count++;
                }
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

/* ── Write SYN options ────────────────────────────────────────────────────── */
int tcp_options_write_syn(uint8_t *buf, size_t bufsz,
                           uint16_t mss, uint8_t wscale,
                           bool sack_perm, bool timestamps, uint32_t ts_val)
{
    uint8_t *p = buf;
    size_t  remaining = bufsz;

#define NEED(n) if (remaining < (n)) goto done; remaining -= (n);

    /* MSS */
    NEED(4);
    p[0] = TCPOPT_MSS; p[1] = 4;
    p[2] = (uint8_t)(mss >> 8); p[3] = (uint8_t)mss;
    p += 4;

    /* SACK Permitted */
    if (sack_perm) {
        NEED(2);
        p[0] = TCPOPT_SACK_PERM; p[1] = 2;
        p += 2;
    }

    /* Timestamps */
    if (timestamps) {
        NEED(10);
        p[0] = TCPOPT_TIMESTAMP; p[1] = 10;
        p[2] = (uint8_t)(ts_val>>24); p[3] = (uint8_t)(ts_val>>16);
        p[4] = (uint8_t)(ts_val>>8);  p[5] = (uint8_t)ts_val;
        p[6] = p[7] = p[8] = p[9] = 0; /* ts_ecr = 0 for SYN */
        p += 10;
    }

    /* Window Scale */
    NEED(3);
    p[0] = TCPOPT_NOP; p++;
    p[0] = TCPOPT_WINDOW_SCALE; p[1] = 3; p[2] = wscale;
    p += 3;

done:
    /* Pad to 4-byte boundary with NOP */
    while ((p - buf) % 4 != 0 && remaining > 0) {
        *p++ = TCPOPT_NOP;
        remaining--;
    }
#undef NEED
    return (int)(p - buf);
}

/* ── Write data/ACK options ──────────────────────────────────────────────── */
int tcp_options_write_data(uint8_t *buf, size_t bufsz,
                            bool timestamps, uint32_t ts_val, uint32_t ts_ecr,
                            const sack_block_t *sack, uint8_t sack_count)
{
    uint8_t *p = buf;
    size_t   remaining = bufsz;

#define NEED(n) if (remaining < (n)) goto done; remaining -= (n);

    if (timestamps) {
        NEED(10);
        p[0] = TCPOPT_NOP; p[1] = TCPOPT_NOP;
        p[2] = TCPOPT_TIMESTAMP; p[3] = 10;
        p[4] = (uint8_t)(ts_val>>24); p[5] = (uint8_t)(ts_val>>16);
        p[6] = (uint8_t)(ts_val>>8);  p[7] = (uint8_t)ts_val;
        p[8] = (uint8_t)(ts_ecr>>24); p[9] = (uint8_t)(ts_ecr>>16);
        /* overwrite p[10..11] with more ts_ecr bytes */
        NEED(2);
        p[10] = (uint8_t)(ts_ecr>>8); p[11] = (uint8_t)ts_ecr;
        p += 12;
    }

    if (sack && sack_count > 0) {
        uint8_t sb_count = sack_count > 4 ? 4 : sack_count;
        uint8_t opt_len  = (uint8_t)(2 + sb_count * 8);
        NEED(opt_len);
        p[0] = TCPOPT_NOP; p[1] = TCPOPT_NOP;
        p[2] = TCPOPT_SACK; p[3] = opt_len;
        uint8_t *s = p + 4;
        for (uint8_t i = 0; i < sb_count; i++) {
            uint32_t l = sack[i].left, r = sack[i].right;
            s[0]=(uint8_t)(l>>24); s[1]=(uint8_t)(l>>16);
            s[2]=(uint8_t)(l>>8);  s[3]=(uint8_t)l;
            s[4]=(uint8_t)(r>>24); s[5]=(uint8_t)(r>>16);
            s[6]=(uint8_t)(r>>8);  s[7]=(uint8_t)r;
            s += 8;
        }
        p += opt_len;
    }

done:
    while ((p - buf) % 4 != 0 && remaining > 0) {
        *p++ = TCPOPT_NOP;
        remaining--;
    }
#undef NEED
    return (int)(p - buf);
}
