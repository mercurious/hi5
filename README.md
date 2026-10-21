# HI5

A 5-voice FM glitch beat generator for the Miyoo Mini Plus running OnionOS.

It has four FM percussion voices (SUB, KICK, SNARE, PERC) and a HAT voice, which is a
micro-loop sampler. Each voice is sequenced by three polymetric Euclidean cells: HIT, STUT
(ratchets) and GLCH (variations). On top of that are punch-in performance FX, a tempo-synced
resonant filter sweep, a reverb bus, pattern and kit memory, a song chain, a mono + clock-pulse
output for syncing other gear, and Wi-Fi tempo sync between units.

## Install

### Over Wi-Fi

Turn on SSH on the Miyoo (Apps > Tweaks > Network > SSH). Then run this on a computer on the
same network:

```sh
curl -fsSL https://raw.githubusercontent.com/mercurious/hi5/main/install.sh | bash
```

The script finds the Miyoo by scanning the local /24 for Onion's SSH server. It checks the
installed version against the latest
[release](https://github.com/mercurious/hi5/releases), and if the release is newer it copies
the update to `SD:/App/HI5`. Your `hi5.sav` and samples are kept. Run it again any time to
update. Log in with Onion's default `onion` / `onion`. The address that worked is saved in
`~/.config/hi5/host`. To skip the scan, set `HI5_HOST`:

```sh
curl -fsSL https://raw.githubusercontent.com/mercurious/hi5/main/install.sh | HI5_HOST=192.168.1.28 bash
```

`HI5_USER` changes the SSH user. `HI5_FORCE=1` reinstalls even when the version matches.
Quit HI5 on the Miyoo before updating.

### From the SD card

1. Build it (see below), or download `HI5.zip` from the latest release.
2. Copy the `HI5` folder to `SD:/App/HI5` on the OnionOS card. With the card mounted, you can
   also run `make install SD=/run/media/$USER/<card>`.
3. Launch **HI5** from Apps. Press MENU to quit.

The state autosaves to `hi5.sav` in the app folder.

### Your own hat samples

Put `.wav` files in `App/HI5/samples/`: 8/16/24/32-bit PCM or 32-bit float, mono or stereo,
any sample rate, up to 3 s each. They load in filename order after the built-in sounds
(CHAT, OHAT, PEDAL, SHAKR, RIDE). Pick them on the SYNTH page under HAT > SAMPLE.

### Audio crackle

`launch.sh` passes `HI5_BUFFER` (default 1024) and `HI5_RATE` (default 48000) through to
the app. If you hear crackle, try `HI5_BUFFER=2048` or `HI5_RATE=44100`.

## Controls

These work on every page:

| Button | Action |
| --- | --- |
| L1 / R1 | Previous / next page |
| START | Play / stop |
| L2 (hold) | Tempo-synced resonant filter sweep |
| SELECT | Cycle the L2 sweep rate (1/4, 1/2, 1 bar, 2 bars) |
| R2 + d-pad | Play the punch-in FX in slots 1-4 |
| R2 + X | Tap tempo |
| MENU | Quit |

The pages are PATTERN, MIX, SYNTH, FX and SONG. The footer of each page shows its own keys.

- **PATTERN** shows a 5 × 3 grid of cells. Each voice has a HIT, STUT and GLCH cell.
  Use the d-pad to select a cell, A to toggle it, B for fewer hits, Y to re-permute the
  Euclidean rhythm and X to rotate it.
- **MIX** has a column per voice plus a master column. Left / right picks the column,
  A picks the row and up / down edits it. A voice column has LEVEL, PAN, DRIVE and SEND
  (reverb). The master column has LEVEL, DRIVE, VERB (DIGITAL / SPRING), TIME and TONE.
  Y mutes the voice and X solos it.
- **SYNTH** edits each voice's parameters. Use the d-pad to select, Y / X for - / + and
  A to audition. The FM voices have TUNE, RATIO, INDEX, DECAY, SWEEP, FEEDBK, NOISE
  (DRIVE on SUB), STUTTR and GLITCH. The hat has SAMPLE, START, LOOP, PITCH, TONE, DRIFT,
  RESO and SHINE.
- **FX** sets up the four R2 slots. Each slot has a type, two parameters and a mix. The
  types are REPEAT, TAPESTOP, REVERSE, CRUSH, GATE, DELAY and GRAIN. The OUT row switches
  between stereo and mono + clock, and sets the clock side, PPQN (1-32) and pulse width.
- **SONG** has eight pattern slots (A-H), eight kits (synth params + mix balance), a 16-step
  chain and the Wi-Fi sync panel.
  - On a pattern slot or kit: A loads, X saves, Y copies a pattern slot and R2+Y clears.
  - On a chain node: up / down picks the slot, A cycles the bars (1/2/4/8), X pastes,
    Y copies, R2+Y clears and R2+A plays the song from that node.
  - B toggles song mode and loop mode. R2+B hops between the slots and the chain.

## Wi-Fi sync

HI5 units on the same network share tempo, bar phase and transport over UDP broadcast on
port 5150. Set the mode on the SONG page's sync panel: OFF, AUTO, LEAD or FOLLOW. A unit set
to LEAD wins. Otherwise the unit with the lowest id leads. OnionOS Wi-Fi must be on.
The sibling drone synth Jaffedrone uses the same protocol.

## Building

You need [zig](https://ziglang.org/) 0.14 for the cross-compile. The Makefile's default path
is `~/.local/opt/zig-aarch64-linux-0.14.1/zig`; override it with `ZIG=...`. You also need
`curl` and `python3`. `make test` needs a host C compiler and ImageMagick (`magick`).

```sh
make              # cross-compile dist/HI5/hi5 (ARMv7 hard-float, glibc 2.28)
make test         # host harness: render screenshots and audio into build/test
make zip          # package dist/HI5 as build/HI5.zip
make install SD=/path/to/mounted/sdcard
make build/hi5_synctest   # host-side Wi-Fi sync interop check
```

The build links against Onion's own MI_AO/MI_GFX-backed SDL 1.2. On the first build, `make`
downloads it into `third_party/onion-lib/`, which isn't committed.

`src/font_data.h` and `dist/HI5/icon.png` are generated files. The scripts that make them
are `tools/mkfont.py` (from Noto Sans Mono Bold) and `tools/mkicon.py`.

### Layout

| Path | Contents |
| --- | --- |
| `src/engine.c` | Voices, sequencer, FX, reverb, save / load |
| `src/ui.c` | Pages, input, drawing |
| `src/gfx.c` | Framebuffer and text drawing |
| `src/sync.c` | Wi-Fi sync |
| `src/main_sdl.c` | Device entry point (SDL audio + video) |
| `src/main_test.c` | Host test harness |
| `src/main_synctest.c` | Host sync interop test |
| `dist/HI5/` | The app folder as installed on the card |

## License

MIT. See [LICENSE](LICENSE). The SDL 1.2 headers in `third_party/` and Onion's SDL library
are under the LGPL. See `third_party/SDL-1.2.15-headers/COPYING`.
