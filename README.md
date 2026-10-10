# fault-inject

Deterministic fault injection for embedded C. Script the failures your
bench never shows you — flipped bits, dropped frames, stuck sensors,
timing jitter — against plain byte buffers, then assert your code lands
in a safe state.

Every run takes a fixed seed, so a failure replays bit-for-bit.

## What's here (session 1)

- `src/fi.h`, `src/fi.c` — the injector core: seeded PRNG, fault arming,
  frame-level decisions (drop / duplicate), payload corruption (bit flip,
  byte corrupt, stuck-at, truncate), and a virtual clock for jitter.
- `demo/reasm.h`, `demo/reasm.c` — a small fragmented-message reassembler
  (J1939-TP-style connection + data packets), the "device under test".
- `demo/safety_demo.c` — six scenarios: clean run, dropped packet,
  corrupted length field, stuck sequence byte, truncated frame, jitter
  bounds. Each prints its verdict.

## Build and run

```sh
make          # builds ./safety_demo
./safety_demo
```

Strict build: `gcc -Wall -Wextra -Werror -std=c11 -pedantic`. No
dependencies, no hardware, no malloc.

## License

MIT — see LICENSE.
