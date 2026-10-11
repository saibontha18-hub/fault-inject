# fault-inject

Deterministic fault injection for embedded C. Script the failures your
bench never shows you — flipped bits, dropped frames, stuck sensors,
timing jitter — against plain byte buffers, then assert your code lands
in a safe state.

Every run takes a fixed seed, so a failure replays bit-for-bit. Print the
session report when something goes wrong and you get a replayable record
for free.

## What's here

- `src/fi.h`, `src/fi.c` — the injector: seeded PRNG (xorshift64*), fault
  arming, frame-level decisions (drop / duplicate), payload corruption
  (bit flip, byte corrupt, stuck-at, truncate), `fi_clear` to disarm
  mid-campaign, and a virtual clock for timing jitter without sleeping.
- `demo/reasm.h`, `demo/reasm.c` — a small fragmented-message reassembler
  (J1939-TP-shaped: one connection packet announces the transfer, then
  sequenced data packets), the "device under test".
- `demo/safety_demo.c` — seven scenarios: clean run, dropped packet,
  corrupted packet-count field, stuck sequence byte, truncated frame,
  jitter bounds, and a replay check (same seed, same faults → identical
  verdict, twice).
- `tests/test_fi.c` — self-contained suite, no frameworks, just a CHECK
  macro: 18 groups, 481 checks. Every fault kind, edge cases (empty
  frames, a full fault table, stuck-at expiry, truncate bounds), PRNG
  determinism, jitter monotonicity, replay identity, and the reassembler
  driven under faults.

## The idea in 30 seconds

```c
fi_t f;
fi_init(&f, 42);                        /* fixed seed — replayable */

fi_fault_t drop = { .kind = FI_DROP, .frame = 3 };
fi_arm(&f, drop);                       /* swallow the 4th frame whole */

/* ...per frame on the wire: */
if (fi_decide(&f, frame, len) == FI_DROPPED)
    continue;                           /* never arrives */
len = fi_corrupt(&f, frame, len);       /* bit flips etc., where armed */
fi_advance(&f, 1000);                   /* 1 ms nominal + vclock jitter */
```

Target one frame (or `FI_ANY_FRAME` for all of them), one byte (or
`FI_ANY_POS` and let the PRNG pick), one bit. `fi_report(&f, stdout)`
dumps the seed, the armed faults, and what fired — paste it into the bug
report.

## Build and test

```sh
make            # builds ./safety_demo
make test       # full suite + demo, both must pass
./safety_demo   # the seven scenarios, with verdicts
```

Strict build: `gcc -Wall -Wextra -Werror -std=c11 -pedantic`. No
dependencies, no hardware, no malloc — it runs on a host exactly as it
would against a byte buffer on target.

## Why this exists

The embedded failures I actually worry about aren't logic bugs — they're
the bus misbehaving at 2 AM. A bit flips on a noisy line, a frame gets
lost in a brownout, a sensor wedges at one value and nobody notices for
a week. You can't reproduce those on the bench by hoping. This lets you
script them, deterministically, and prove your receiver lands somewhere
safe instead of somewhere weird.

## License

MIT — see LICENSE.
