/*
 * safety_demo — what happens to a transport reassembly when the bus
 * misbehaves. Each scenario arms a fault with a FIXED seed, so the run is
 * replayable: same seed, same corruption, same verdict.
 */
#include <stdio.h>
#include <string.h>

#include "reasm.h"
#include "../src/fi.h"

#define NPKTS 6   /* 1 connection + 5 data packets */

/* Build a transfer of 5 data packets carrying a known pattern. */
static void build_transfer(uint8_t pkts[NPKTS][8], uint16_t total)
{
    uint8_t count = (uint8_t)((total + REASM_PAYLOAD - 1) / REASM_PAYLOAD);
    int i, j;

    memset(pkts[0], 0xFF, 8);
    pkts[0][0] = REASM_CMD_CONN;
    pkts[0][1] = (uint8_t)(total & 0xFF);
    pkts[0][2] = (uint8_t)(total >> 8);
    pkts[0][3] = count;

    for (i = 1; i <= count; i++) {
        pkts[i][0] = (uint8_t)i;
        for (j = 0; j < REASM_PAYLOAD; j++)
            pkts[i][1 + j] = (uint8_t)(i * 16 + j);
    }
}

/* Run one transfer through the fault injector into the reassembler. */
static reasm_status_t run(fi_t *f, uint8_t pkts[NPKTS][8], size_t *out_len)
{
    reasm_t r;
    reasm_status_t st = REASM_OK;
    uint8_t frame[8];
    size_t flen;
    int i;

    reasm_reset(&r);
    for (i = 0; i < NPKTS && st == REASM_OK; i++) {
        fi_decision_t d;
        int copies, c;

        memcpy(frame, pkts[i], 8);
        flen = 8;

        d = fi_decide(f, frame, flen);
        if (d == FI_DROPPED)
            continue;                       /* frame never arrives */
        copies = (d == FI_DUP) ? 2 : 1;

        flen = fi_corrupt(f, frame, flen);
        fi_advance(f, 1000);                /* 1 ms nominal frame time */
        for (c = 0; c < copies && st == REASM_OK; c++)
            st = reasm_feed(&r, frame, flen, out_len);
    }
    return st;
}

static const char *verdict(reasm_status_t st)
{
    return st == REASM_DONE ? "COMPLETE"
         : st == REASM_ERR  ? "REJECTED (safe state)"
                            : "INCOMPLETE (timed out, no crash)";
}

int main(void)
{
    uint8_t pkts[NPKTS][8];
    fi_t f;
    fi_fault_t ft;
    size_t out_len = 0;
    reasm_status_t st;

    printf("fault-inject safety demo — transport reassembly under faults\n");
    printf("============================================================\n");

    /* 1. Clean run: everything arrives, message completes. */
    build_transfer(pkts, 30);
    fi_init(&f, 1);
    st = run(&f, pkts, &out_len);
    printf("[1] no faults        -> %-28s len=%zu\n", verdict(st), out_len);

    /* 2. Drop data packet 3: sender thinks it went out, receiver waits. */
    build_transfer(pkts, 30);
    fi_init(&f, 42);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DROP;
    ft.frame = 3;                 /* 0-based: conn=0, so this is data seq 3 */
    fi_arm(&f, ft);
    st = run(&f, pkts, &out_len);
    printf("[2] drop packet 3    -> %-28s (safe: never completes)\n",
           verdict(st));

    /* 3. Bit flip inside the connection packet's packet-count field:
     * the sender claims 5 packets, the wire says 1. */
    build_transfer(pkts, 30);
    fi_init(&f, 7);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    ft.frame = 0;
    ft.byte_pos = 3;
    ft.bit = 2;                   /* count 5 -> 1 */
    ft.count = 1;
    fi_arm(&f, ft);
    st = run(&f, pkts, &out_len);
    printf("[3] corrupt pkt count -> %-28s (safe: lies rejected)\n",
           verdict(st));

    /* 4. Sequence byte stuck at 1: every data packet claims to be first. */
    build_transfer(pkts, 30);
    fi_init(&f, 99);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_STUCK_AT;
    ft.frame = FI_ANY_FRAME;
    ft.byte_pos = 0;
    ft.value = 1;
    ft.count = 10;
    fi_arm(&f, ft);
    st = run(&f, pkts, &out_len);
    printf("[4] stuck seq byte   -> %-28s\n", verdict(st));

    /* 5. Truncated data packet: short frame on the wire. */
    build_transfer(pkts, 30);
    fi_init(&f, 1234);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_TRUNCATE;
    ft.frame = 2;
    ft.count = 3;
    fi_arm(&f, ft);
    st = run(&f, pkts, &out_len);
    printf("[5] truncated frame  -> %-28s\n", verdict(st));

    /* 6. Timing jitter: virtual clock must stay monotonic and bounded. */
    {
        uint64_t t0, t1;
        int ok = 1, i;
        fi_init(&f, 555);
        fi_jitter_arm(&f, 250);          /* up to +250 us per frame */
        t0 = fi_now(&f);
        for (i = 0; i < 20; i++) {
            fi_advance(&f, 1000);
            t1 = fi_now(&f);
            if (t1 <= t0 || t1 - t0 > 1250)
                ok = 0;
            t0 = t1;
        }
        printf("[6] jitter +/-250us  -> %s\n",
               ok ? "CLOCK OK (monotonic, bounded)" : "CLOCK BROKEN");
    }

    printf("============================================================\n");
    printf("done. rerun with the same seeds to replay any scenario.\n");
    return 0;
}
