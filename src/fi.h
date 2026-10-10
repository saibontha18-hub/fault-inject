/*
 * fault-inject — deterministic fault injection for embedded C.
 *
 * The idea is simple: embedded code fails in the field because of things
 * that never happen on the bench — a flipped bit on a noisy bus, a dropped
 * frame during a brownout, a sensor stuck at one value. This library lets
 * you script those faults against byte buffers and frame streams, with a
 * fixed seed so every failure is replayable.
 *
 * Nothing here touches hardware. Faults are applied to plain buffers; the
 * virtual clock gives you timing jitter without sleeping.
 */
#ifndef FI_H
#define FI_H

#include <stddef.h>
#include <stdint.h>

#define FI_ANY_FRAME (-1)  /* fault applies to every frame, not just one */
#define FI_ANY_POS   (-1)  /* pick the byte/bit position from the PRNG */
#define FI_MAX_FAULTS 32

/* What kind of bad luck to inject. */
typedef enum {
    FI_DROP,         /* swallow the frame whole — receiver never sees it */
    FI_DUPLICATE,    /* deliver the frame twice in a row */
    FI_BIT_FLIP,    /* flip one bit (or `count` bits) in the payload */
    FI_BYTE_CORRUPT,/* overwrite one byte with a random value */
    FI_STUCK_AT,    /* pin one byte to `value` for `count` frames */
    FI_TRUNCATE,    /* chop `count` bytes off the tail of the frame */
} fi_kind_t;

typedef struct {
    fi_kind_t kind;
    int       frame;     /* 0-based frame index, or FI_ANY_FRAME */
    int       byte_pos;  /* byte to hit, or FI_ANY_POS for random */
    int       bit;       /* bit to flip (0-7), or FI_ANY_POS for random */
    uint8_t   value;     /* stuck-at pin value */
    int       count;     /* stuck-at duration / bits to flip / bytes to chop */
} fi_fault_t;

typedef struct {
    uint64_t seed;
    uint64_t prng;                 /* xorshift64* state */
    int      frame_no;             /* frames seen so far */
    fi_fault_t faults[FI_MAX_FAULTS];
    int        nfaults;
    uint64_t vclock_us;            /* virtual clock, microseconds */
    int      jitter_us;            /* max extra delay per frame, 0 disables */
    int      fired;                /* total faults fired (for the report) */
} fi_t;

/* Frame-level verdict. Call fi_decide() once per outgoing/incoming frame. */
typedef enum {
    FI_PASS,      /* deliver normally */
    FI_DROPPED,  /* fault swallowed it — do not deliver */
    FI_DUP,      /* deliver this frame, then deliver it again */
} fi_decision_t;

void fi_init(fi_t *f, uint64_t seed);
int  fi_arm(fi_t *f, fi_fault_t fault);   /* 0 on success, -1 if table full */

fi_decision_t fi_decide(fi_t *f, const uint8_t *frame, size_t len);

/* Payload-level corruption. Returns the new length (truncate shrinks it).
 * Safe to call on every frame — faults only fire where they were armed. */
size_t fi_corrupt(fi_t *f, uint8_t *buf, size_t len);

/* Timing. Arm a max jitter, then advance the clock once per frame; fi_now()
 * reads it. No sleeping involved — the "delay" is just a number, so tests
 * stay fast and deterministic. */
void     fi_jitter_arm(fi_t *f, int max_us);
void     fi_advance(fi_t *f, uint64_t nominal_us);
uint64_t fi_now(const fi_t *f);

/* Raw deterministic randomness, exposed so tests can verify repeatability. */
uint32_t fi_rand32(fi_t *f);

#endif /* FI_H */
