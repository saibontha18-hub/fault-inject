/*
 * fault-inject test suite. Self-contained, no frameworks — just a CHECK
 * macro and a nonzero exit on failure. Compiles under the same strict
 * flags as the library.
 */
#include <stdio.h>
#include <string.h>

#include "../src/fi.h"
#include "../demo/reasm.h"

static int checks = 0;
static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) {                                                  \
            failures++;                                                 \
            printf("FAIL line %d: %s\n", __LINE__, #cond);               \
        }                                                               \
    } while (0)

static int popcount8(uint8_t x)
{
    int n = 0;
    while (x) {
        n += (int)(x & 1u);
        x >>= 1;
    }
    return n;
}

/* Same seed -> identical PRNG stream; different seed -> different. */
static void test_prng_deterministic(void)
{
    fi_t a, b;
    int i, same = 1, diff = 0;

    fi_init(&a, 12345);
    fi_init(&b, 12345);
    for (i = 0; i < 64; i++)
        if (fi_rand32(&a) != fi_rand32(&b))
            same = 0;
    CHECK(same);

    fi_init(&a, 1);
    fi_init(&b, 2);
    for (i = 0; i < 64; i++)
        if (fi_rand32(&a) != fi_rand32(&b))
            diff = 1;
    CHECK(diff);

    /* Seed 0 must still work (no degenerate all-zero stream). */
    fi_init(&a, 0);
    CHECK(fi_rand32(&a) != 0 || fi_rand32(&a) != 0);
}

/* The fault table holds FI_MAX_FAULTS entries, then refuses. */
static void test_arm_full(void)
{
    fi_t f;
    fi_fault_t ft;
    int i;

    fi_init(&f, 1);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    for (i = 0; i < FI_MAX_FAULTS; i++)
        CHECK(fi_arm(&f, ft) == 0);
    CHECK(fi_arm(&f, ft) == -1);
}

/* fi_clear disarms everything mid-campaign; the virtual clock keeps going. */
static void test_clear(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t frame[4] = { 1, 2, 3, 4 };
    uint64_t t;

    fi_init(&f, 99);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DROP;
    ft.frame = 0;
    fi_arm(&f, ft);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_DROPPED);
    CHECK(f.fired == 1);

    fi_advance(&f, 1000);
    t = fi_now(&f);
    fi_clear(&f);
    CHECK(f.nfaults == 0);
    CHECK(f.fired == 0);
    CHECK(fi_now(&f) == t);   /* clock untouched */
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);

    /* Clearing an empty table is harmless. */
    fi_clear(&f);
    CHECK(f.nfaults == 0);
}

/* Fixed-position bit flip hits exactly the right bit, nothing else. */
static void test_bit_flip_exact(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t buf[8];
    size_t n;

    fi_init(&f, 9);
    memset(buf, 0, sizeof(buf));
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    ft.frame = 0;
    ft.byte_pos = 2;
    ft.bit = 4;
    ft.count = 1;
    CHECK(fi_arm(&f, ft) == 0);

    n = fi_corrupt(&f, buf, sizeof(buf));
    CHECK(n == sizeof(buf));
    CHECK(buf[2] == 0x10);
    CHECK(buf[0] == 0 && buf[1] == 0 && buf[3] == 0);
    CHECK(f.fired == 1);
}

/* Random single-bit flip changes exactly one bit in the whole buffer. */
static void test_bit_flip_random_single(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t buf[16], before[16];
    size_t n;
    int i, bits = 0;

    fi_init(&f, 77);
    for (i = 0; i < 16; i++)
        buf[i] = (uint8_t)(i * 3 + 1);
    memcpy(before, buf, sizeof(buf));
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    ft.frame = FI_ANY_FRAME;
    ft.byte_pos = FI_ANY_POS;
    ft.bit = FI_ANY_POS;
    ft.count = 1;
    fi_arm(&f, ft);

    n = fi_corrupt(&f, buf, sizeof(buf));
    CHECK(n == sizeof(buf));
    for (i = 0; i < 16; i++)
        bits += popcount8((uint8_t)(buf[i] ^ before[i]));
    CHECK(bits == 1);
}

/* Byte corrupt fires and keeps the length. */
static void test_byte_corrupt(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t buf[8];
    size_t n;

    fi_init(&f, 5);
    memset(buf, 0xAA, sizeof(buf));
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BYTE_CORRUPT;
    ft.frame = 0;
    ft.byte_pos = 0;
    fi_arm(&f, ft);

    n = fi_corrupt(&f, buf, sizeof(buf));
    CHECK(n == sizeof(buf));
    CHECK(f.fired == 1);
    /* bytes 1..7 untouched no matter what random value byte 0 got */
    CHECK(buf[1] == 0xAA && buf[7] == 0xAA);
}

/* Stuck-at pins the byte for `count` frames, then releases it. */
static void test_stuck_at(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t buf[4];

    fi_init(&f, 3);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_STUCK_AT;
    ft.frame = FI_ANY_FRAME;
    ft.byte_pos = 1;
    ft.value = 0x5A;
    ft.count = 2;
    fi_arm(&f, ft);

    memset(buf, 0, sizeof(buf));
    fi_corrupt(&f, buf, sizeof(buf));
    CHECK(buf[1] == 0x5A);
    memset(buf, 0, sizeof(buf));
    fi_corrupt(&f, buf, sizeof(buf));
    CHECK(buf[1] == 0x5A);
    memset(buf, 0, sizeof(buf));
    fi_corrupt(&f, buf, sizeof(buf));
    CHECK(buf[1] == 0x00);   /* expired — byte left alone */
    CHECK(f.fired == 2);
}

/* Truncate shortens; it never grows or empties via count >= len. */
static void test_truncate(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t buf[8];

    fi_init(&f, 11);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_TRUNCATE;
    ft.frame = 0;
    ft.count = 3;
    fi_arm(&f, ft);
    CHECK(fi_corrupt(&f, buf, 8) == 5);

    fi_init(&f, 11);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_TRUNCATE;
    ft.frame = 0;
    ft.count = 8;             /* chopping the whole frame is a no-op */
    fi_arm(&f, ft);
    CHECK(fi_corrupt(&f, buf, 8) == 8);

    /* Empty buffer is safe. */
    CHECK(fi_corrupt(&f, buf, 0) == 0);
}

/* Drop swallows exactly the armed frame; neighbors pass. */
static void test_drop(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t frame[4] = { 1, 2, 3, 4 };

    fi_init(&f, 21);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DROP;
    ft.frame = 1;
    fi_arm(&f, ft);

    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);
    fi_advance(&f, 1000);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_DROPPED);
    fi_advance(&f, 1000);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);
}

/* Duplicate fires once for the armed frame; ANY_FRAME form is ignored. */
static void test_duplicate(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t frame[4] = { 1, 2, 3, 4 };

    fi_init(&f, 31);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DUPLICATE;
    ft.frame = 2;
    fi_arm(&f, ft);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);
    fi_advance(&f, 1000);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);
    fi_advance(&f, 1000);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_DUP);

    fi_init(&f, 31);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DUPLICATE;
    ft.frame = FI_ANY_FRAME;
    fi_arm(&f, ft);
    CHECK(fi_decide(&f, frame, sizeof(frame)) == FI_PASS);
}

/* Jitter keeps the virtual clock monotonic and bounded; 0 means exact. */
static void test_jitter(void)
{
    fi_t f;
    uint64_t prev, now;
    int i;

    fi_init(&f, 555);
    fi_jitter_arm(&f, 250);
    prev = fi_now(&f);
    for (i = 0; i < 200; i++) {
        fi_advance(&f, 1000);
        now = fi_now(&f);
        CHECK(now > prev);
        CHECK(now - prev <= 1250);
        prev = now;
    }

    fi_init(&f, 555);
    fi_jitter_arm(&f, 0);
    fi_advance(&f, 1000);
    CHECK(fi_now(&f) == 1000);
}

/* Same seed + same faults + same input = byte-identical corruption. */
static void test_replay_identical(void)
{
    fi_t a, b;
    fi_fault_t ft;
    uint8_t x[16], y[16];
    int i;

    for (i = 0; i < 16; i++) {
        x[i] = (uint8_t)i;
        y[i] = (uint8_t)i;
    }
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    ft.frame = FI_ANY_FRAME;
    ft.byte_pos = FI_ANY_POS;
    ft.bit = FI_ANY_POS;
    ft.count = 3;

    fi_init(&a, 2026);
    fi_arm(&a, ft);
    fi_corrupt(&a, x, sizeof(x));

    fi_init(&b, 2026);
    fi_arm(&b, ft);
    fi_corrupt(&b, y, sizeof(y));

    CHECK(memcmp(x, y, sizeof(x)) == 0);
}

/* --- integration: faults against the transport reassembler --- */

static void build_transfer(uint8_t pkts[6][8], uint16_t total)
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

/* Drive all 6 packets through fi_decide/fi_corrupt into the reassembler. */
static reasm_status_t drive(fi_t *f, uint8_t pkts[6][8], size_t *out_len)
{
    reasm_t r;
    reasm_status_t st = REASM_OK;
    uint8_t frame[8];
    size_t flen;
    int i, c, copies;
    fi_decision_t d;

    reasm_reset(&r);
    for (i = 0; i < 6 && st == REASM_OK; i++) {
        memcpy(frame, pkts[i], 8);
        flen = 8;
        d = fi_decide(f, frame, flen);
        if (d == FI_DROPPED)
            continue;
        copies = (d == FI_DUP) ? 2 : 1;
        flen = fi_corrupt(f, frame, flen);
        fi_advance(f, 1000);
        for (c = 0; c < copies && st == REASM_OK; c++)
            st = reasm_feed(&r, frame, flen, out_len);
    }
    return st;
}

static void test_reasm_clean(void)
{
    fi_t f;
    uint8_t pkts[6][8];
    size_t out = 0;

    build_transfer(pkts, 30);
    fi_init(&f, 1);
    CHECK(drive(&f, pkts, &out) == REASM_DONE);
    CHECK(out == 30);
}

static void test_reasm_drop_never_completes(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t pkts[6][8];
    size_t out = 0;

    build_transfer(pkts, 30);
    fi_init(&f, 42);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DROP;
    ft.frame = 3;
    fi_arm(&f, ft);
    /* No crash, no completion — the receiver just waits. */
    CHECK(drive(&f, pkts, &out) == REASM_OK);
}

static void test_reasm_lying_count_rejected(void)
{
    fi_t f;
    fi_fault_t ft;
    uint8_t pkts[6][8];
    size_t out = 0;

    build_transfer(pkts, 30);
    fi_init(&f, 7);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_BIT_FLIP;
    ft.frame = 0;
    ft.byte_pos = 3;
    ft.bit = 2;
    ft.count = 1;
    fi_arm(&f, ft);
    CHECK(drive(&f, pkts, &out) == REASM_ERR);
}

static void test_reasm_oversize_conn_rejected(void)
{
    reasm_t r;
    uint8_t pkt[8] = { REASM_CMD_CONN, 0xFF, 0xFF, 10, 0, 0, 0, 0 };
    size_t out = 0;

    reasm_reset(&r);
    /* 65535 announced bytes — must be refused, not allocated. */
    CHECK(reasm_feed(&r, pkt, sizeof(pkt), &out) == REASM_ERR);
}

static void test_reasm_seq_skip_rejected(void)
{
    reasm_t r;
    uint8_t pkts[6][8];
    size_t out = 0;
    reasm_status_t st;

    build_transfer(pkts, 30);
    reasm_reset(&r);
    st = reasm_feed(&r, pkts[0], 8, &out);
    CHECK(st == REASM_OK);
    st = reasm_feed(&r, pkts[1], 8, &out);
    CHECK(st == REASM_OK);
    /* Skip seq 2, jump straight to seq 3. */
    st = reasm_feed(&r, pkts[3], 8, &out);
    CHECK(st == REASM_ERR);
}

/* The session report prints the seed and the armed faults. */
static void test_report(void)
{
    fi_t f;
    fi_fault_t ft;
    char buf[1024];
    FILE *tmp;

    fi_init(&f, 4242);
    memset(&ft, 0, sizeof(ft));
    ft.kind = FI_DROP;
    ft.frame = 1;
    fi_arm(&f, ft);
    fi_advance(&f, 1000);

    tmp = tmpfile();
    CHECK(tmp != NULL);
    if (tmp) {
        fi_report(&f, tmp);
        rewind(tmp);
        CHECK(fread(buf, 1, sizeof(buf) - 1, tmp) > 0);
        buf[sizeof(buf) - 1] = '\0';
        CHECK(strstr(buf, "4242") != NULL);
        CHECK(strstr(buf, "DROP") != NULL);
        fclose(tmp);
    }
}

int main(void)
{
    test_prng_deterministic();
    test_arm_full();
    test_clear();
    test_bit_flip_exact();
    test_bit_flip_random_single();
    test_byte_corrupt();
    test_stuck_at();
    test_truncate();
    test_drop();
    test_duplicate();
    test_jitter();
    test_replay_identical();
    test_reasm_clean();
    test_reasm_drop_never_completes();
    test_reasm_lying_count_rejected();
    test_reasm_oversize_conn_rejected();
    test_reasm_seq_skip_rejected();
    test_report();

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
