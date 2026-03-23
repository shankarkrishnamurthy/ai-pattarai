/* SPDX-License-Identifier: BSD-3-Clause
 * vaigAI: TCP send buffer allocation.
 */
#include "tcp_snd_buf.h"
#include <rte_malloc.h>

tcp_snd_buf_t *
tcp_snd_buf_alloc(uint32_t cap)
{
    if (cap == 0) cap = TCP_SND_BUF_DEFAULT_CAP;

    tcp_snd_buf_t *sb = rte_malloc("tcp_snd_buf", sizeof(*sb), 0);
    if (!sb) return NULL;

    sb->data = rte_malloc("tcp_snd_buf_data", cap, 0);
    if (!sb->data) {
        rte_free(sb);
        return NULL;
    }
    sb->cap = cap;
    sb->len = 0;
    return sb;
}

void
tcp_snd_buf_free(tcp_snd_buf_t *sb)
{
    if (!sb) return;
    rte_free(sb->data);
    rte_free(sb);
}
