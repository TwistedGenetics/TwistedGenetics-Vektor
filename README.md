# Vektor

A classic **vector synthesis** oscillator for the **Korg microKORG 2**, built with the logue SDK.

Four waveforms sit on the corners of a square, and an X/Y point mixes between them, the way the Sequential Prophet VS and Korg Wavestation did it. What makes it a vector synth rather than a crossfader is the **vector path**: every note sets the X/Y point moving on its own route around the square, so each note evolves from one timbre into another while you hold it. Paths restart on every note and run per voice, so chords shimmer as each voice travels independently.

```
  A ---------- B        VectorX: left (A, C) -> right (B, D)
  |            |        VectorY: bottom (C, D) -> top (A, B)
  |     +      |
  |            |        every point inside is a blend of all four
  C ---------- D
```

All 16 waves are band-limited (no aliasing even at the top of the keyboard) and loudness-matched, so moving between corners doesn't jump in level.

## Download & install

Go to [**Releases**](https://github.com/TwistedGenetics/TwistedGenetics-Vektor/releases/latest) and download **`Vektor.mk2unit`**.

Needs microKORG 2 firmware **2.0.0 or later** (the version that added logue SDK support).

- **KORG Sound Librarian**: connect the microKORG 2 by USB, add `Vektor.mk2unit` as a user oscillator and send it to the synth.
- **Or USB mass storage:** hold the first option button while powering on, then copy the file into `Units/Oscs/` (into a `SlotN` folder) on the drive that appears.

Vektor then appears at the end of the oscillator list as **Vektor**. It's polyphonic, with an independent vector path per voice.

## Controls

| Page | Parameter | Range | What it does |
|---|---|---|---|
| 1 | VECTORX | 0–1023 | Where the mix sits left (A, C) to right (B, D). The path moves around this point |
| 1 | VECTORY | 0–1023 | Where the mix sits bottom (C, D) to top (A, B) |
| 1 | PATH | 10 paths | The per-note vector movement (see below). **Off** = a static mix set by the knobs |
| 2 | WAVE A | 16 waves | Top-left corner |
| 2 | WAVE B | 16 waves | Top-right corner |
| 2 | WAVE C | 16 waves | Bottom-left corner |
| 2 | WAVE D | 16 waves | Bottom-right corner |
| 2 | DETUNE | 0–100 % | Spreads the corners up to 12 cents apart (A stays, B up, C down, D half up). Adds movement and width to blends |
| 3 | PATHRATE | 0–100 | Path speed: about 4 s per segment at 0, ~20 ms at 100 |
| 3 | PATHAMT | 0–100 % | How far the path moves the X/Y point. 100 % with the knobs centred covers the whole square |
| 3 | BRIGHT | 0–100 | Key-tracked low-pass (12 dB/oct) that takes the edge off bright waves. 0 = almost a sine, 35 = default (cutoff about 14× the note), 100 = off (raw, bright waves). Because it tracks the note, it sounds the same across the keyboard |

### The paths

| Path | Movement |
|---|---|
| **Off** | No movement: the knobs (and the virtual patch) set the mix |
| **A>D** | One-shot: starts on corner A, glides diagonally to D and stays there. The classic evolving pad |
| **D>A** | The same, reversed |
| **Tour** | One-shot: A → B → D → C, then settles in the centre |
| **Circle** | Loops round the square, starting at the top |
| **Fig 8** | Loops a figure of eight across the square |
| **Bounce** | Loops left ↔ right (A/C ↔ B/D) at the VectorY height |
| **Diamond** | Loops through the four edges (pure two-wave blends at each point) |
| **Wander** | Smoothly drifts between random points, different for every voice |
| **RndNote** | No movement, but every note lands on a different random point: per-note timbre variation |

One-shot paths restart on every note; with a slow PATHRATE they're like a timbre envelope. Looped paths at high rates turn into an audio-ish wobble.

### The waves

| | | | |
|---|---|---|---|
| **Sine** | **Tri** | **Saw** | **Square** |
| **Pulse** (25 %) | **Narrow** (10 % pulse) | **Organ** (drawbars) | **Glass** (bell partials) |
| **Vox A** ("ah" formants) | **Vox O** ("oh" formants) | **Brass** | **Reed** (hollow, clarinet-ish) |
| **Comb** (notched, phasey) | **Metal** (digital clang) | **Sync** (hard-sync saw) | **Fold** (wavefolded sine) |

Mixing two *different* types across a diagonal is where vector synthesis shines: a soft wave (Sine, Vox, Organ) against a hard one (Sync, Metal, Saw).

### Virtual patch

Vektor adds five modulation destinations: **VectorX**, **VectorY**, **PathAmt**, **PathRate** and **Bright**, all per voice. These sit on top of the path. Ideas:

- **Mod wheel → VectorY**: play the path, then push the whole route up toward A/B.
- **LFO → VectorX** with PATH Off: a classic two-axis wobble without using a path.
- **Velocity → PathAmt**: soft notes stay near the knob position, hard notes travel further.
- **Velocity → Bright**: harder notes open up, like a velocity-sensitive filter that tracks the keyboard.
- **Envelope → PathRate**: paths that start fast and slow down.

## Starter settings

| Name | X | Y | Path | A | B | C | D | Detune | Rate | Amt | Bright |
|---|---|---|---|---|---|---|---|---|---|---|---|
| **Init (default)** | 512 | 512 | Circle | Saw | Vox A | Glass | Organ | 20 | 30 | 70 | 35 |
| **VS Pad** | 512 | 512 | A>D | Vox O | Glass | Organ | Saw | 35 | 20 | 100 | 30 |
| **Digital Tour** | 512 | 512 | Tour | Metal | Sync | Comb | Fold | 10 | 45 | 100 | 50 |
| **Choir Drift** | 512 | 512 | Wander | Vox A | Vox O | Reed | Tri | 50 | 25 | 90 | 35 |
| **Wobble Bass** | 512 | 300 | Bounce | Saw | Square | Pulse | Narrow | 15 | 70 | 80 | 25 |
| **Brass Eight** | 512 | 512 | Fig 8 | Brass | Saw | Square | Organ | 25 | 40 | 75 | 40 |

Every build renders these as WAV demos (in the Actions build artifact, `test/out/`).

## Building

The GitHub Actions workflow builds `Vektor.mk2unit` with Korg's toolchain, runs the DSP tests and publishes a Release whenever the `.version` in `microkorg2/header.c` is bumped.

To build locally (Linux x86_64 or aarch64):

```
./build.sh          # -> dist/Vektor.mk2unit
./test/run.sh       # desktop DSP tests + WAV demos in test/out/ (any OS, no toolchain needed)
```

`build.sh` downloads the logue SDK and Korg's microKORG 2 toolchain into `.deps/` on first run.

## How it works

- **Wave tables:** each wave is defined by its harmonic spectrum and built at load time (inverse FFT) into 9 band-limited levels (255 harmonics down to 1). Each level is long enough for at least 32 samples per cycle of its top harmonic (8192 samples down to 1024), so the linear interpolation between table samples doesn't add fizz: inharmonic noise is 73 dB or more below the signal on every wave. Each corner picks the level that keeps its top harmonic under 21 kHz. Nothing is stored in the unit file.
- **Mix:** bilinear weights across the four corners, with a (Σw²)^-¼ gain so the centre of the square isn't 6 dB quieter than the corners.
- **Smoothing:** X/Y glides over ~3 ms and weights ramp across each buffer, so knob jumps and fast paths don't click.
- **CPU:** the path, weights and table level are worked out once per buffer per voice; the sample loop is four interpolated table reads. A corner with zero weight isn't read at all.

Source layout: `core/vektor.h` (DSP), `microkorg2/` (logue SDK unit), `test/` (desktop tests).

## Unit ID

Developer ID `0x5477476E` ('TwGn'), unit ID 0x10 (16), kept well clear of the IDs other TwistedGenetics units use, so they can all be loaded together.

## Changes

- **1.1.1**: darker default: BRIGHT now starts at 35 (was 60), which takes the raspy top edge off the raw waves. Starter settings updated to match. Patches saved with 1.1.0 keep their own BRIGHT value.
- **1.1.0**: fixes the buzzy, fizzy sound of 1.0.0. The wave tables were too short (512 samples) for the number of harmonics in them, so reading between table samples added inharmonic noise only 35–55 dB down, worst on bass notes. Tables are now 1024–8192 samples, which puts that noise 73 dB or more down. Also: new **BRIGHT** control (key-tracked low-pass, default 60) and Bright modulation destination, softer gaps between the Vox formants, and corner D defaults to Organ instead of Sync.
- **1.0.0**: first release.

## License

BSD 3-Clause, see [LICENSE](LICENSE).
