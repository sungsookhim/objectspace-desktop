# G0 results (2026-11-18)

Prototypes for the GUI design ([docs/gui-design.md](../../docs/gui-design.md)), run on Fedora with GNOME Shell 51.1 under Wayland, monitor at scale 1 and 60 Hz.

- `folderview`: a details-view folder window drawn with our own code (Cairo, HarfBuzz, FreeType, fontconfig, xkbcommon) on a plain `xdg-shell` surface, with a square, Warp 4-style frame drawn by the client (GNOME does not draw frames for Wayland clients). It reads the folder from `osdd` on a service thread, a page at a time, while the drawing thread only reads the model under a short lock.
- `soundlat`: a PipeWire stream kept open writing silence, playing a click when an action asks for it.
- `bench.sh`: runs `folderview --bench` cold, warm, and at 2× scale against a fresh `osdd`.

Build with `make g0`. Run by hand: `build/osdd &` then `build/g0/folderview ~/some/folder`.

## Folder window over 1,000,000 entries

The benchmark scrolls by itself for 20 s: a fling of three rows per frame for 3 s, then a jump to a random place every 0.5 s, as if the scroll bar thumb were dragged. The folder is `<big>/million` from the M4 scenarios. Window 760 × 560 unless stated.

| Case | First rows | All names in | fps | Draw p50 / p99 / max | Frame interval p99 / max | Late frames | Frames with placeholder rows | Frames with rows waiting for size and date |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Cold cache, 1× | 6.7 ms | 2.1 s | 59.9 | 3.7 / 5.7 / 8.9 ms | 18.4 / 25.4 ms | 1 of 1,199 | 8 | 49 |
| Warm cache, 1× | 1.6 ms | 0.5 s | 59.9 | 4.0 / 5.8 / 6.7 ms | 17.8 / 35.7 ms | 2 of 1,199 | 22 | 11 |
| Warm cache, 2× | 1.4 ms | 0.5 s | 59.9 | 5.7 / 9.2 / 11.7 ms | 17.4 / 29.0 ms | 1 of 1,199 | 26 | 7 |
| Cold cache, 1×, 1920 × 1048 | 2.9 ms | 0.9 s | 59.9 | 3.6 / 6.0 ms | 18.7 / 38.5 ms | 1 of 1,199 | 33 | (not counted yet) |
| Warm cache, 2×, 1920 × 1048 | | | **46.7** | **14.5 / 18.4 / 28.7 ms** | 33.4 / 71.5 ms | 66 of 701 | | |

Findings:

- **The service path keeps up.** Names stream in faster than anyone scrolls (a million in 0.5 s warm, 2 s cold); rows waiting for details appear for a frame or two after a jump and are drawn as grey bars, never blocking.
- **One process drawing with Cairo on the CPU holds 60 fps at 1× and at 2× for ordinary window sizes.** HarfBuzz shaping of every visible string every frame is not a measurable cost.
- **A near-full-screen window at 2× (7.6 million pixels) does not: 14.5 ms a frame, 47 fps.** Redrawing the whole window every frame is the cause. G1's widget set must:
  - repaint only damaged areas: when scrolling, copy the rows already drawn within the buffer and draw only the newly exposed ones; the frame, headings and status line are drawn once and only when they change;
  - keep a third buffer so a frame never waits for the compositor to release one.
  The aim is under 4 ms a frame at 2× full screen; G1 measures it again.
- **Late frames** (one or two in 1,200) line up with the jumps' first frame, not with draw time; the 25–38 ms maxima are single frames. Not worth chasing before the damage-based drawing.

## Sound latency

30 clicks at irregular intervals, the stream open and running beforehand, `node.latency` 256/48000.

| | p50 | p99 | max |
| --- | --- | --- | --- |
| Action to the sound being written into the stream | 2.4–3.6 ms | 5.2 ms | 5.2 ms |

The only output on the test machine is PipeWire's Dummy Output, so the device's own delay (its buffer, typically 5–20 ms) could not be measured and reported as zero. With the graph's quantum at 1024 (21 ms at 48 kHz) the worst case on real hardware could be about 26 ms; the sound service will ask for a 256-frame quantum (5.3 ms), which PipeWire grants when the device allows it. Measure on real speakers in G3.

## Bugs found in the object core

- **Socket paths longer than 107 bytes were cut short silently** (a socket address holds 107 bytes plus the terminator). `osdd` removed a stale socket at the full path but bound to the shortened one, so after one crash it refused to start ("Address already in use"). `osdd`, `osdctl`, `osd-watchd` and the tracker now refuse such a path with a clear message.
