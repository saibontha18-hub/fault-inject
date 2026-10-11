#include "fi.h"

#include <stdio.h>

/* xorshift64*: small, fast, and — critically — deterministic. A seeded
 * PRNG means a failing fault run can be replayed bit-for-bit by reusing
 * the seed in the report. No rand(), no time(NULL), no surprises. */
static uint64_t xorshift64star(uint64_t *state)
{
    uint64_t x = *state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *state = x;
    return x * 2685821657736338717ULL;
}

void fi_init(fi_t *f, uint64_t seed)
{
    f->seed = seed;
    /* xorshift needs nonzero state; fold the seed so 0 still works. */
    f->prng = seed ^ 0x9E3779B97F4A7C15ULL;
    if (f->prng == 0)
        f->prng = 0x2545F4914F6CDD1DULL;
    f->frame_no = 0;
    f->nfaults = 0;
    f->vclock_us = 0;
    f->jitter_us = 0;
    f->fired = 0;
}

int fi_arm(fi_t *f, fi_fault_t fault)
{
    if (f->nfaults >= FI_MAX_FAULTS)
        return -1;
    f->faults[f->nfaults++] = fault;
    return 0;
}

void fi_clear(fi_t *f)
{
    /* Mid-campaign reset: drop every armed fault and zero the fired
     * count, but leave frame_no and the virtual clock alone — whatever
     * is happening on the wire keeps happening, just without faults. */
    f->nfaults = 0;
    f->fired = 0;
}

uint32_t fi_rand32(fi_t *f)
{
    return (uint32_t)(xorshift64star(&f->prng) >> 32);
}

/* Does fault `ft` apply to the current frame? */
static int applies(const fi_t *f, const fi_fault_t *ft)
{
    return ft->frame == FI_ANY_FRAME || ft->frame == f->frame_no;
}

static size_t pick_pos(fi_t *f, const fi_fault_t *ft, size_t len)
{
    if (ft->byte_pos != FI_ANY_POS)
        return (size_t)ft->byte_pos;
    return (size_t)(fi_rand32(f) % (uint32_t)len);
}

fi_decision_t fi_decide(fi_t *f, const uint8_t *frame, size_t len)
{
    int i;

    (void)frame;
    (void)len;

    for (i = 0; i < f->nfaults; i++) {
        const fi_fault_t *ft = &f->faults[i];
        if (!applies(f, ft))
            continue;
        if (ft->kind == FI_DROP) {
            f->fired++;
            return FI_DROPPED;
        }
        if (ft->kind == FI_DUPLICATE) {
            /* Explicit-frame only: duplicating FI_ANY_FRAME would double
             * every frame, which is just a weird bus. */
            if (ft->frame != FI_ANY_FRAME) {
                f->fired++;
                return FI_DUP;
            }
        }
    }
    return FI_PASS;
}

size_t fi_corrupt(fi_t *f, uint8_t *buf, size_t len)
{
    int i;

    if (len == 0)
        return 0;

    for (i = 0; i < f->nfaults; i++) {
        fi_fault_t *ft = &f->faults[i];
        size_t pos;
        int b;

        if (!applies(f, ft))
            continue;

        switch (ft->kind) {
        case FI_BIT_FLIP:
            /* count defaults to 1 if the caller left it at 0. */
            for (b = 0; b < (ft->count > 0 ? ft->count : 1); b++) {
                pos = pick_pos(f, ft, len);
                if (ft->bit != FI_ANY_POS)
                    buf[pos] ^= (uint8_t)(1u << (ft->bit & 7));
                else
                    buf[pos] ^= (uint8_t)(1u << (fi_rand32(f) & 7u));
            }
            f->fired++;
            break;

        case FI_BYTE_CORRUPT:
            pos = pick_pos(f, ft, len);
            buf[pos] = (uint8_t)fi_rand32(f);
            f->fired++;
            break;

        case FI_STUCK_AT:
            /* Pin the byte for `count` frames, then the fault expires on
             * its own. Stuck sensors are the classic field failure. */
            if (ft->count > 0) {
                pos = pick_pos(f, ft, len);
                buf[pos] = ft->value;
                ft->count--;
                f->fired++;
            }
            break;

        case FI_TRUNCATE:
            if ((size_t)ft->count < len) {
                len -= (size_t)ft->count;
                f->fired++;
            }
            break;

        case FI_DROP:
        case FI_DUPLICATE:
            /* frame-level faults, handled in fi_decide(). */
            break;
        }
    }
    return len;
}

void fi_jitter_arm(fi_t *f, int max_us)
{
    f->jitter_us = max_us > 0 ? max_us : 0;
}

void fi_advance(fi_t *f, uint64_t nominal_us)
{
    uint64_t extra = 0;

    f->frame_no++;
    if (f->jitter_us > 0)
        extra = fi_rand32(f) % (uint32_t)(f->jitter_us + 1);
    f->vclock_us += nominal_us + extra;
}

uint64_t fi_now(const fi_t *f)
{
    return f->vclock_us;
}

static const char *kind_name(fi_kind_t k)
{
    switch (k) {
    case FI_DROP:         return "DROP";
    case FI_DUPLICATE:    return "DUPLICATE";
    case FI_BIT_FLIP:    return "BIT_FLIP";
    case FI_BYTE_CORRUPT: return "BYTE_CORRUPT";
    case FI_STUCK_AT:    return "STUCK_AT";
    case FI_TRUNCATE:     return "TRUNCATE";
    }
    return "?";
}

void fi_report(const fi_t *f, FILE *out)
{
    int i;

    fprintf(out, "fault-inject session report\n");
    fprintf(out, "  seed: %llu\n", (unsigned long long)f->seed);
    fprintf(out, "  frames: %d  virtual time: %llu us  faults fired: %d\n",
            f->frame_no, (unsigned long long)f->vclock_us, f->fired);
    fprintf(out, "  armed faults (%d):\n", f->nfaults);
    for (i = 0; i < f->nfaults; i++) {
        const fi_fault_t *ft = &f->faults[i];
        fprintf(out, "    [%d] %-12s frame=%d byte=%d bit=%d value=0x%02x count=%d\n",
                i, kind_name(ft->kind), ft->frame, ft->byte_pos, ft->bit,
                ft->value, ft->count);
    }
    fprintf(out, "  replay: fi_init(&f, %llu) and arm the same faults in order.\n",
            (unsigned long long)f->seed);
}
