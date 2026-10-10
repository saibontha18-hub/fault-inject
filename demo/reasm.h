/*
 * reasm — a minimal fragmented-message reassembler, the "device under test"
 * for the fault-inject demo.
 *
 * It borrows the shape of J1939 transport protocol (BAM): one connection
 * packet announces the transfer (total length, packet count), then data
 * packets stream in with sequence numbers. Deliberately plain C over byte
 * buffers — the point is to show what faults do to a real state machine,
 * not to model any particular bus.
 */
#ifndef REASM_H
#define REASM_H

#include <stddef.h>
#include <stdint.h>

#define REASM_MAX_MSG 2048   /* hard cap — no unbounded allocation, ever */
#define REASM_PAYLOAD 7      /* payload bytes per data packet */

/* Packet layout */
#define REASM_CMD_CONN 0x20
#define REASM_CONN_LEN 8
#define REASM_DATA_LEN 8

typedef enum {
    REASM_OK,    /* packet accepted, transfer still in progress */
    REASM_DONE,  /* message complete — out_len holds the payload size */
    REASM_ERR,   /* rejected: bad seq, length overrun, protocol violation */
} reasm_status_t;

typedef struct {
    uint8_t  buf[REASM_MAX_MSG];
    uint16_t total_len;   /* announced by the connection packet */
    uint8_t  pkt_count;   /* announced packet count */
    uint8_t  next_seq;    /* sequence number we expect next (1-based) */
    uint16_t received;    /* payload bytes collected so far */
    int      active;      /* 1 while a transfer is open */
} reasm_t;

void reasm_reset(reasm_t *r);

/* Feed one raw packet (REASM_CONN_LEN or REASM_DATA_LEN bytes).
 * On REASM_DONE, *out_len is the reassembled message length. */
reasm_status_t reasm_feed(reasm_t *r, const uint8_t *pkt, size_t len,
                          size_t *out_len);

#endif /* REASM_H */
