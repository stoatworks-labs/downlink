# downlink

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The satellite link is not
> asserted but measured: an offline harness drives the real plugin class and the
> real shaders it runs, and checks each claim against a closed form. The
> discriminator's clicks, counted in its own output, land on the exact slip rate of
> a sampled discriminator at four carrier-to-noise ratios across the threshold, and
> on Rice's formula when it is sampled finely enough. Near white, the negative clicks
> outnumber the positive ones by what Rice's modulation term predicts. Above the knee
> the output SNR tracks CNR one dB for one, 20.6 dB above it; the knee sits within
> 0.2 dB of where Rice and Campbell put it, and threshold extension moves it by what
> the model says. The dispersal triangle comes through each clamp exactly as
> predicted, pixel by pixel, at two rasters. The emphasis networks meet ITU-R F.405
> within its own tolerance. Eight negative controls prove the checks can fail. It has
> **never been loaded into Resolume on macOS**, where it is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host and is
> not Resolume. On Windows, a build of v0.1.0 loads, registers and renders in Resolume
> Arena 7.27.1 with every control as declared, on software rendering. See
> [Status](#status).

Analogue FM satellite television, as an FFGL effect for [Resolume](https://resolume.com)
Arena and Avenue.

![Colour bars, a grey ramp and a white box on black, received over a satellite link just below threshold: short white and black horizontal streaks scattered over the picture, black ones on the white box and white ones on the black, a few whole lines a shade brighter or darker where the clamp caught a click on the porch](docs/hero.png)

<sub>One frame, rendered by `dltest`, the offline harness — not captured from
Resolume. The test card at CNR 7.5 dB, just under the knee, with the defaults
otherwise: PAL, 13.5 MHz/V, a 27 MHz IF, Clamp Good.</sub>

## The one idea

Analogue satellite TV sent its composite video as **frequency modulation** of a
carrier, because FM trades bandwidth for noise. That trade has a cliff. Above the FM
threshold the picture is clean, with a little fine coloured grain. Below it the
discriminator starts throwing **clicks**: whenever the noise swings the received
phasor around the origin, the demodulated phase jumps by a whole cycle. Each click is
a short impulse, smeared by the de-emphasis and the video lowpass into a horizontal
streak. Those are the **sparklies** a dish slightly off-pointing, or a rain fade,
filled the picture with.

The plugin builds the link, **PAL → pre-emphasis → FM carrier with energy dispersal →
noisy channel → Gaussian IF → discriminator → lowpass → de-emphasis → clamp → PAL
decoder**, on the GPU, per sample, in parallel. The discriminator runs at 283.75 MHz,
sixteen times the video rate. What reaches the screen is whatever the receiver made
of it.

## What falls out

None of these is drawn. Each is the link doing what it does:

- **Fine coloured grain above threshold.** FM noise rises with frequency, so after the
  de-emphasis its worst lands at the top of the video band, on the chroma subcarrier,
  and the PAL decoder turns it into colour.
- **Output SNR tracks CNR one-for-one**, 20.6 dB above it at the defaults, then
  collapses: from the knee (8.7 dB CNR here) to 3 dB under it the picture loses 9.7 dB.
- **Sparklies, at Rice's rate.** A few dozen a field at the default 9 dB, thousands at
  4 dB, a handful at 10.
- **Black sparklies on white, white ones on black.** A bright picture holds the carrier
  off the IF's centre, and the extra clicks are all of the sign that pulls it back:
  Rice's modulation term.
- **Whole lines jumping.** A keyed clamp that happens to sample a click on the back
  porch lifts or drops the entire line.
- **The picture rocking.** The uplink sweeps its carrier with a 25 Hz triangle (energy
  dispersal). `Clamp` Good takes it off every line; Slow lags it and the picture
  flickers field against field; Off leaves it all, and the brightness rocks by most of
  its range at the field rate.
- **Threshold extension.** `Demodulator` Threshold Ext. narrows the noise the
  discriminator sees by 3 dB, and the knee drops by 3.9.
- **Rain and music.** `Rain Fade` takes dB off the link; `Audio Fade` does it by the
  level of the audio routed in.

### The honest limit

The receiver is one design. The IF is Gaussian rather than any particular SAW filter.
Threshold extension is an ideal tracking filter, not a PLL, so it has no modulation
term. The decoder's colour reference is perfect: there is no burst, so no hue error
from noise on it. The discriminator is sampled, and a sampled discriminator misses
clicks that come and go within a sample: 3.6% of Rice's rate at 4 dB, 11% at 10 dB. The
harness predicts exactly those. Component mode is three separate carriers, an
idealised MAC. Nobody has measured how Resolume lays out its 64 FFT bins, so
`Audio Fade` reads their total level and nothing else.

## Controls

| Group | |
| --- | --- |
| **Uplink** | Deviation (4–24 MHz/V at the pre-emphasis crossover), Dispersal (0–8 MHz p-p), Pre-emphasis On (CCIR 405). |
| **Link** | CNR (0–25 dB in the IF), Rain Fade (0–10 dB), Audio Fade (up to 12 dB at full level), IF Bandwidth (18–36 MHz), Noise Seed. |
| **Receiver** | Demodulator (Discriminator, Threshold Ext.), Clamp (Off, Slow, Good), De-emphasis On, Video Bandwidth (2–6 MHz). |
| **Signal** | Composite (PAL, Component), Mix. |

The defaults are a full-transponder PAL link a little under par: 13.5 MHz/V, 2 MHz of
dispersal, a 27 MHz IF, 9 dB CNR, a good clamp. The picture is clean but for a few
dozen sparklies a field. Pull CNR down 2 dB and they take over.

## Status

**v0.1.0, and honestly early. 23 September 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4) against a fresh
universal Release build, running every check at **two rasters**, 320×180 and
1280×720. What it establishes, in numbers:

| check | result |
| --- | --- |
| `--emphasis` | both networks within **0.019 dB** of ITU-R F.405-1's 625-line curve from 0.01 to 5 MHz (its tolerance is 0.10–0.15 dB); pre- then de-emphasis flat to **0.018 dB** across 576 tones from 0.05 to 5.05 MHz (bound 0.021, the two networks' departures summed) |
| `--clicks` | on an unmodulated carrier, **9,115 / 3,407 / 2,031 / 257** clicks at 4 / 6 / 8 / 10 dB, counted in the discriminator's output, against the sampled discriminator's exact **8,983 / 3,378 / 1,975 / 257** (4σ Poisson: 381 / 234 / 179 / 64); at 64× the sampling, **286,302 /s** against Rice's continuous-time **286,665 /s** |
| `--polarity` | near white, positive to negative clicks **0.279** against **0.268** at 6 dB and **0.198** against **0.200** at 8 dB (tolerance 0.030, 0.027) |
| `--threshold` | SNR − CNR measured within **0.08 dB** of the chain's own **20.59 dB** from 14 to 23 dB, **1.007 dB per dB**; the knee at **8.71 dB** against **8.57** (tolerance 0.59, derived from the model's two approximations); Threshold Ext.'s knee at 4.87 against 4.67, moved **3.84 dB** against **3.90** |
| `--dispersal` | with Clamp Off every pixel of a flat grey within **1.8e-5** of the triangle (tolerance 1.1e-4); Slow within 6.0e-4 of a first-order lag (tolerance 1.35e-3); Good within 1.4e-6, its residual 1.7e-3 under the 2.1e-3 per-line bound |
| `--identity` | a noiseless component link returns four saturated quadrants to the same 8-bit code (worst 0; one code allowed) away from the edges |
| `--seed`, `--resize` | the noise is bit-identical for a seed and field, and at both rasters; another seed or field differs; a resize mid-run leaves nothing behind |
| `--negative` | eight broken models, each failing the check that should catch it: a float clock, a pre-emphasis pole off F.405, a linearised discriminator (no clicks), the noise rotated the wrong way, the Slow clamp's time constant skipped, a de-emphasis pole off, a receiver wrong about the deviation (twice) |
| mutation | one character of the shipped link shader (`1.0 + m.x` to `1.0 - m.x`, which reflects the noise) was caught by `--polarity` alone, at both rasters, then reverted |
| `tools/sweep.py` | all **14** swept controls measurably change the picture |
| shaders | all 12 compile through `glslc`, not merely through Apple's driver |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Downlink` / `DL01` / `effect` and renders 120 frames through `plugMain` |

Render cost at the defaults, best of three runs of 30 frames after a warm-up,
`glFinish` both sides, on a GPU shared with other builds:

| | PAL ms/frame | % of a 60fps frame | Component ms/frame |
| --- | --- | --- | --- |
| 1280×720 | 3.1 | 18.5% | 5.8 |
| 1920×1080 | 3.2 | 19.0% | 5.9 |
| 3840×2160 | 3.7 | 22.3% | 6.5 |

The link runs at its own raster whatever the output size, so the output size hardly
matters; the numbers move with whatever else the GPU was doing. The discriminator is
most of it (1.5 ms by timer query for PAL, 4.2 ms for Component's three carriers).
macOS figures only.

### In Resolume, on Windows

**Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, 2026-09-24): a CI build of
v0.1.0 loads from Extra Effects, registers as `SW Downlink` / `DL01` / effect, all 21
host controls match the declaration in name, order, type, range and default, it
renders, and Arena's log stays clean: 9 of 9 of the fleet gate's checks. 11 controls
moved the picture, 2 of them under a precondition. Deviation, Dispersal and IF
Bandwidth read inconclusive, because the link's noise changes every frame; the two
audio controls, Audio and Audio Fade, could not be tested, because win-lab has no
sound device. Software rendering says nothing about a GPU or about speed.

### Not established

It has **never been loaded into Resolume on macOS**. Everything above the Windows
section was compiled, rendered and measured offline against the real plugin class in
a headless CGL context, plus an `oxbow` load. Still untested:

- how 15 controls in four groups, one of them the audio input, present in Arena's
  inspector on a Mac;
- whether 3 ms of GPU (6 for Component) is comfortable beside other effects on a
  show machine;
- what Resolume's FFT bins are, and so how hard `Audio Fade` bites;
- what the host's real clock does over a long session.

The coloured grain falls out of the chain and is visible, but no check measures the
noise spectrum's shape. Nothing has been through a show. There is no OpenFX port. The
[browser demo](https://downlink-demo.stoatworks-labs.com/) runs the plugin's own ten
shaders, but the link's CPU half — the filter design, the noise statistics, the per-row
dispersal table, the clock — is a hand port to JavaScript that only a reader checks,
and a browser has no audio, so Audio Fade does nothing there.

The [user guide](docs/USER-GUIDE.md) covers every control, what it does and why.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/downlink
cd downlink
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default. Add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via
vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic 60 fps
clock:

```bash
./build/dltest --out /tmp/frame.png --size 1920x1080   # the test card, received
./build/dltest --list                                  # every control, kind and default
./build/dltest --offline                               # everything that needs no GL
./build/dltest --emphasis --clicks --polarity          # the link
./build/dltest --threshold --dispersal --identity
./build/dltest --seed --resize --size 320x180          # the picture, at another raster
./build/dltest --negative                              # and the checks can fail
./build/dltest --bench                                 # 720p to 4K, and per pass
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`. Run them at 320×180 as well as the raster you care about.
`--pipe` takes raw RGBA frames on stdin and writes them to stdout, with an optional
cue sheet, for rendering footage through the real shaders.

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model, the derivation and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
