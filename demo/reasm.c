#include "reasm.h"

void reasm_reset(reasm_t *r)
{
    r->total_len = 0;
    r->pkt_count = 0;
    r->next_seq = 0;
    r->received = 0;
    r->active = 0;
}

/* Connection packet: [0x20][len_lo][len_hi][pkt_count][pad x4] */
static reasm_status_t feed_conn(reasm_t *r, const uint8_t *pkt, size_t len)
{
    uint16_t total;
    uint8_t count;

    if (len != REASM_CONN_LEN)
        return REASM_ERR;

    total = (uint16_t)pkt[1] | ((uint16_t)pkt[2] << 8);
    count = pkt[3];

    /* Reject nonsense up front: empty transfers, oversized buffers, and
     * packet counts that can't possibly carry the announced length. */
    if (total == 0 || total > REASM_MAX_MSG)
        return REASM_ERR;
    if (count == 0)
        return REASM_ERR;
    if ((uint16_t)(count - 1) * REASM_PAYLOAD >= total)
        return REASM_ERR;

    r->total_len = total;
    r->pkt_count = count;
    r->next_seq = 1;
    r->received = 0;
    r->active = 1;
    return REASM_OK;
}

/* Data packet: [seq][7 payload bytes] */
static reasm_status_t feed_data(reasm_t *r, const uint8_t *pkt, size_t len,
                                size_t *out_len)
{
    uint8_t seq = pkt[0];
    size_t take;

    if (len != REASM_DATA_LEN)
        return REASM_ERR;

    /* Strict sequencing: out-of-order, repeated, or skipped packets are
     * protocol violations, not silent data corruption. */
    if (seq != r->next_seq)
        return REASM_ERR;
    if (r->next_seq > r->pkt_count)
        return REASM_ERR;

    /* Never write past the announced length — a lying packet count can't
     * turn this into a buffer overflow. */
    take = REASM_PAYLOAD;
    if (r->received + take > r->total_len)
        take = r->total_len - r->received;
    if (r->received + take > REASM_MAX_MSG)
        return REASM_ERR;

    for (size_t i = 0; i < take; i++)
        r->buf[r->received + i] = pkt[1 + i];

    r->received += (uint16_t)take;
    r->next_seq++;

    if (r->received >= r->total_len) {
        r->active = 0;
        *out_len = r->total_len;
        return REASM_DONE;
    }
    return REASM_OK;
}

reasm_status_t reasm_feed(reasm_t *r, const uint8_t *pkt, size_t len,
                          size_t *out_len)
{
    if (len == 0)
        return REASM_ERR;

    /* A new connection packet mid-transfer aborts the old one — the sender
     * restarting is not the receiver's problem to untangle. */
    if (pkt[0] == REASM_CMD_CONN)
        return feed_conn(r, pkt, len);

    if (!r->active)
        return REASM_ERR;

    return feed_data(r, pkt, len, out_len);
}
