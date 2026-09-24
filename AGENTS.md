# AGENTS.md — Downlink

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

Analogue FM satellite television, as an FFGL 2.1 effect (`DL01`, shown as
`SW Downlink`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and a Windows `.dll`. MIT, public at
`github.com/stoatworks-labs/downlink`.

Built 2026-09-23 in one session from `~/Projects/resolume/specs/SPEC-downlink.md`,
the brief and its addendum. Templates: old-cathode for the PAL encoder, decoder and
the 4fsc raster; ferric for filtering along the row in scan order; readout and pitch
for the host clock and the per-row table computed in double; tinsel, pitch and
slowscan for the harness, sweep, verify and CI.

---

## The one idea

**Build the link: PAL → pre-emphasis → FM carrier with dispersal → noisy channel →
Gaussian IF → discriminator → lowpass → de-emphasis → clamp → PAL decoder.** FM
trades bandwidth for noise, and the trade has a cliff. Everything the operator sees
is the discriminator doing its maths:

| the chain, doing what it does | what comes out |
| --- | --- |
| Gaussian noise in the IF, an `arg( r[k] conj r[k-1] )` discriminator, above threshold | fine grain; SNR = CNR + 20.6 dB at the defaults, one dB for one dB |
| the FM noise spectrum rising as f², flattened by de-emphasis only below 1.5 MHz | the worst of the grain at the top of the band, on the chroma subcarrier: **coloured** grain |
| the phase error passing ±π below threshold | **clicks**, a whole cycle each, at Rice's rate (the sampled discriminator's exact version of it) |
| a click through the video lowpass and the de-emphasis | a short horizontal streak, white or black: the **sparklies** |
| the carrier held off the IF's centre by a bright picture | more clicks of the sign that pulls it back: **black sparklies on white** (Rice's modulation term) |
| a keyed clamp that sampled a click on the back porch | the **whole line** jumps in brightness |
| the 25 Hz dispersal triangle and no clamp | the picture **rocking** at the field rate, 0.74 of luma p-p at the defaults |
| a leaky clamp (2 ms) | a first-order lag behind the triangle: a ±0.075 flicker, field against field |
| Threshold Ext.: a tracking filter narrowing the noise 3 dB | the knee 3.9 dB lower, and 0.7 dB less grain above it |

### The discriminator in the carrier's frame (the derivation the spec asks for)

The carrier is `exp( j φ )`, φ advancing by `Δφ[l] = 2π f[l] / fs` between fine
samples, where `f` is Deviation × the pre-emphasised video plus the dispersal. The
channel adds circular complex Gaussian noise `n[l]`, shaped by the IF filter `h`:
`n = h * u'` with `u'` white. The discriminator outputs

    d[k] = arg( r[k] conj r[k−1] ),    r = exp( j φ ) + n.

Write the noise in the carrier's frame, `m[k] = n[k] exp( −j φ[k] )`. Then
`r[k] = exp( j φ[k] ) ( 1 + m[k] )`, and **exactly**

    d[k] = wrap( Δφ[k] + ψ[k] − ψ[k−1] ),    ψ = arg( 1 + m ).

ψ is the phase error. When it passes ±π, `wrap` throws a whole cycle: a click.

`m` needs only phase **differences**. White circular noise is rotation-invariant, so
`u'[l] = u[l] exp( j θ[l] )` is as white as `u` for any deterministic θ. Take θ as
the carrier's phase measured from a local origin a few samples back:

    m[k] = exp( −j θ[k] ) Σ_i h[i] u[k−i] exp( j θ[k−i] )

Every θ enters as `θ[k−i] − θ[k]`, a sum of at most the filter's half-length of
`Δφ`s, so the origin cancels: two fragments that both need `ψ[k]` compute the same
number whatever origin each chose, and the discriminator's output telescopes across
fragment boundaries. No phase is ever integrated along a line; no float ever holds
more than a few tens of radians of it. `u[l]` is an integer hash of
(seed, channel, field, row, l), so a fine sample's noise is the same whichever
fragment draws it. That is `DOWNLINK_LINK_LIBRARY` in `source/Shaders.cpp`.

---

## Where the numbers come from

| figure | value | source |
| --- | --- | --- |
| pre-emphasis | `10 log[(1 + 10.21 f²)/(1 + 0.4083 f²)] − 11 dB`, f in MHz | ITU-R F.405-1 Table 1, 625 lines (read from the ITU's own PDF this session). Crossover 1.512 MHz; −11 dB at DC; +2.98 dB at HF |
| its tolerance | ±(0.1 + 0.05 f/fc) dB | F.405-1 recommends 4 |
| deviation | 13.5 MHz/V p-p at the crossover, 4–24 | Intelsat's figure (Astra used 16; F.405's radio relay 8) |
| dispersal | 2 MHz p-p, 25 Hz triangle, 0–8 | the shared-transponder figure |
| IF | Gaussian, −3 dB width 27 MHz, 18–36 | full transponder 27 MHz; the shape is a decision (below) |
| CNR | carrier over the noise in the IF, 0–25 dB, default 9 | 9 dB puts a few dozen clicks in a field |
| video lowpass | Hann-windowed sinc, 65 taps at 8fsc, 2–6 MHz, default 5 | a decision |
| PAL | 4.43361875 MHz, 283.7516 cycles a line, 921 active samples at 4fsc | old-cathode's `Standards.cpp` |
| carrier rest level | 0.2 V above blanking | the midpoint of a 1 V composite (−0.3 to +0.7) |
| threshold extension | 3 dB less noise bandwidth | a decision; 2–4 dB is what the 1990s demodulators claimed |
| Slow clamp | τ = 2 ms | a DC restorer on a capacitor; a decision |

The FM improvement is **not** a constant typed in anywhere. The harness computes it
from the chain's own taps (20.593 dB at the defaults) and, separately, the classical
formula over F.405's analogue network, a brick wall at the video bandwidth and the
Gaussian IF (20.114 dB). The 0.48 dB between them is the discrete chain against the
textbook's idealisation: mostly, it is thought, the real lowpass's transition band,
which lets through less of the f² noise at the band edge than a brick wall, plus the
boxcar's droop and its 2.7% alias. That split is attributed, not measured; the check
asserts the chain's own figure, which is the one the picture has.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Link.{h,cpp}` | Every constant; the F.405 networks (matched z); the video lowpass; the Gaussian IF taps; the dispersal triangle and the per-row table, in double; the subcarrier phase. No GL, so the harness and `--offline` use it too. |
| `source/Controls.h` | What a 0..1 slider means in MHz, dB and MHz/V; the option names. |
| `source/Shaders.{h,cpp}` | Eleven fragment shaders and the vertex. The link library is one macro used by `kLinkShader` and `kLinkProbeShader`. |
| `source/Downlink.{h,cpp}` | The plugin: parameters, the clock, the ten passes, the test hooks, the probe. |
| `source/PassBuffer.*`, `Diag.*` | tinsel's FFGLFBO with the leak fixed; a log file. |
| `tools/dltest/` | The harness: renders, measures, predicts, benchmarks, pipes. |
| `tools/sweep.py`, `tools/verify.sh`, `tools/check-shaders.sh` | No dead controls; all of it; the GLSL through glslc. |
| `demo/` | The browser demo: a copy of every shader, a PORT of the CPU half, the vendored kit. `demo/tools/check_shaders.py` proves the copies have not drifted. |

The passes, in order (`Shaders.h` has the long form): **resample** the clip to
1842 × 576 (the active line at 8fsc); **encode** PAL or component, porches at
blanking, less the carrier's rest level; **preemph**, causal; **link**, four link
samples a fragment; **detect**, the video lowpass at 8fsc and every second sample;
**deemph**, at 4fsc, and the rest level back; **porch**, the clamp window's mean per
row; **clamp**, what comes off each row; **decode**, the clamp applied and PAL or the
component matrix back to RGB at 921 × 576; **output**, bilinear to the host's raster,
and the mix.

### Three sample rates

- **4fsc, 17.73 MHz** for the video (old-cathode's raster): at exactly 4fsc the
  decoder's chroma notch is a three-tap average.
- **8fsc, 35.47 MHz** for the uplink's baseband and the detector's output. The link
  sums its fine samples into link samples, which is a boxcar decimation, and the FM
  noise rises as f² far past the video band. Decimating straight to 4fsc would fold
  the noise from 13–22 MHz into the band: 71% on top of the in-band noise (Gaussian
  IF, before de-emphasis). At 8fsc it is 2.7%, and the harness's prediction carries
  even that exactly. The de-emphasis then runs at 4fsc after the lowpass has
  decimated: 116 taps where 8fsc would need about 230.
- **16 × 4fsc, 283.75 MHz** for the discriminator. See the next section for what
  that costs in clicks.

### Time

Every host frame shows the **field on air** at the host's time, `floor( t / 20 ms )`,
computed in double. The clip is progressive, so the field is drawn with both of the
clip's rows for each field line: row y is 22 lines plus y half-lines into the field.
The dispersal is locked to the frame, as uplinks locked it: an even field ramps from
−pp/2 to +pp/2, an odd one back down, so a clamp-less picture alternates between two
ramps at the field rate and a 60 fps host shows that as a rocking beat. The noise is
redrawn per field, so a 60 fps host shows the same noise on the two frames that fall
in one field about one frame in six.

---

## The sampled discriminator is not Rice's

The first `--clicks` compared the counts with Rice's `r erfc( √ρ )` and passed, but
every count was low: −2.2%, −4.3%, −4.9%, −11% at 4, 6, 8, 10 dB. Only 4σ of Poisson
hid it. The same discriminator run at 64× matched Rice to 0.1%. **A sampled crossing
detector errs in one direction**: when the phasor dips across the negative real axis
and back between two fine samples, it sees no crossing at all.

The fix is not a tolerance. The sampled discriminator slips at sample k exactly when
`|Δφ + ψ[k] − ψ[k−1]| > π`, and that probability is computable: `m[k−1]` is circular
Gaussian, `m[k]` given it is circular Gaussian with mean `c m[k−1]` and variance
`σ²( 1 − |c|² )` (`c` the carrier-frame taps' lag-one correlation), and the phase of a
circular Gaussian about a mean has a closed-form density (the Rician phase law). So
the expected count is a 2-D integral over `m[k−1]` of that density over the arc that
slips. `dltest` computes it (`discreteRates`) and the counts land on it: |132| of 381,
|29| of 234, |56| of 179, |0| of 64. The shortfall from Rice is derived, not
measured: 3.6% at 4 dB and 11.0% at 10 dB at the shipped 16×, 1.0% and 3.0% at 32×,
13.4% and 36.1% at 8×, closing from below as the sampling rises (`--rice`). Rice's
own rate is then checked where it applies, at 64× (286,302 against 286,665 /s).

Why ship 16× and not 32×? The link's cost goes as the square of the rate (more fine
samples, each through a longer IF filter), and it is already the plugin's biggest
pass. The clicks a 16× discriminator misses are the ones a continuous one throws and
takes back within 3.5 ns, which a 5 MHz lowpass would all but cancel anyway.

---

## Traps

Roughly in the order they will bite.

### ☠️ A grid point on the branch cut counts every click one way

The first exact integral got the **total** right and the **split** 10% wrong: at 6 dB
near white it predicted +/− = 0.295, and the shader, a Monte Carlo of the filtered
sequence and a Monte Carlo of the pairs all said 0.263–0.269. The grid over `m[k−1]`
had a row of points exactly on the axis (`zy = 0` is a grid point of `−L + i dz`), and
`atan2( 0, negative )` is `+π`, so the whole row sat on the upper side of the cut and
every one of its slips was counted positive. Cell centres fixed it (0.268 against a
measured 0.279 ± 0.030). The unmodulated case had hidden it: there the misplaced row
only moved clicks between signs, and the check then was on the total. The integral's
own grid error is now estimated from a half-step grid and added to the tolerance.

### ☠️ The line's ends are a step, and the step is not the physics

`--dispersal` first failed on 43,200 pixels, all in the first and last few dozen
columns: the porches are at blanking, so the active line starts and ends on a step
that every filter smears inwards (the decoder's ±8 taps reach 8 samples into the
porch). Both `--dispersal` and `--identity` now leave out everything within the sum of
the filters' supports of an edge: 163 video samples.

### ☠️ Two fields, not one

The first `--dispersal` note predicted a row-mean swing of 0.69 and measured 0.74. The
check was right and the note was wrong: it rendered an even field and an odd one, and
the swing runs from the bottom of one to the bottom of the other, not from the top of
one field to its bottom. The note now takes its prediction from the same per-pixel
model the check uses.

### ☠️ A float clock only bites across a field boundary

The first `--clock` negative control (take the six-day host time in float) did not
fail. The row table depends on the host time only through the field index, so
rounding 7.0123 s to 7.0 s changed nothing. It fails when the rounding crosses a field
boundary, which float's 31 ms step at six days does one time in ~1.6; the check uses
7.049 s, which float rounds 13.5 ms up into the next field. The float-clock trap is
real (the parity flips and the triangle inverts) but narrower than it looks.

### ☠️ Re-encoding inside the pre-emphasis loop

The first transmit pass encoded PAL inside the 54-tap pre-emphasis loop: a texel
fetch, a matrix and a sincos 54 times per link sample, 0.78 ms of GPU. Encoding once
into its own buffer and filtering that is 0.3 ms.

### ☠️ The shared GPU makes the wall clock lie

Other builds share this GPU. The same bench read 4.9 ms at 4K and 13.9 ms at 1080p in
one run. `--bench` reports best-of-three wall clock AND per-pass GPU time from timer
queries. Apple's GL reports some short passes as 0.000 ms, so the per-pass figures are
a lower bound.

### ☠️ The test window must fit the arrays at every rate the tests use

The link's arrays hold `P Sub + 1 + 2 NoiseHalf` fine samples. At the 64× the
convergence check uses, four link samples a fragment would need 219 of the 128. The
plugin packs fewer link samples per run (`PerRun`) when a test's rate would overflow;
GLSL does not bounds-check, and an overflow would not have announced itself.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
put back before the output); every `ffglex::Scoped*` clears to 0 on exit, so every
`Ensure()` happens before anything binds a texture; `FFGLFBO::Release()` leaks the
colour texture, which is why `PassBuffer::Destroy()` deletes it first; `SetParamInfo`
clamps a STANDARD default into 0..1; the core is an **OBJECT** library;
`SetTextParameter` must return `FF_SUCCESS` for the About block;
`FFGLScopedFBOBinding.h` is not in the umbrella header; the harness drives a
synthetic clock; `nm | grep -q` fails under pipefail when grep succeeds; an option's
range reads back 0..1 whatever its element count; Resolume's clock overflows a float;
a GitHub macOS runner cannot create an accelerated GL context; nobody has measured
Resolume's FFT bins (Audio Fade reads the RMS level only); GLSL `sample`, `input`,
`filter` and friends are reserved (none is used).

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted. `verify.sh` runs every GL
check at 320×180 and 1280×720. The link's checks read the signal buffers, which run at
the link's own 2002 × 576 whatever the host's raster, so they are raster-free by
construction; the question for them is "on another GPU", where GLSL gives `sin`,
`log` and `atan` loose precision. That moves the noise's realisation, never its
statistics, and every tolerance on the link is statistical.

| check | what it measures | tolerance and where it comes from | raster / platform dependence |
| --- | --- | --- | --- |
| `--emphasis` F.405 | each network's response from its truncated taps vs F.405's curve, 0.01–5 MHz | F.405's own ±(0.1 + 0.05 f/fc) dB; worst 0.0011 dB (pre) and 0.0188 dB (de) | none: CPU, double |
| `--emphasis` flat | pre × de, 576 tones 0.05–5.05 MHz, fitted by least squares from the demodulated video | the two networks' departures from F.405 summed (0.0199 dB) + 0.001 dB of fitting; worst 0.0181 dB | none: signal buffer |
| `--clicks` rate | clicks counted in the discriminator's output vs the sampled discriminator's exact rate, 4/6/8/10 dB | 4σ Poisson on the expected count + a third of the integral's change on halving its grid | none: signal buffer. Per-row index of dispersion is 1.19 at 4 dB (clicks cluster), so 4σ is really ~3.7σ there |
| `--clicks` continuous | the same at 64×, vs Rice's `r erfc( √ρ )`, 4 dB | 4σ Poisson; the 64× shortfall extrapolates to ~0.25%, a tenth of 4σ | none |
| `--polarity` | each sign's count and the ratio near white, 6 and 8 dB, vs the exact rates for the offset carrier | 4σ Poisson per sign; the ratio's by the delta method; + the integral's grid term | none |
| `--threshold` offset | SNR (0.7 V over the rms noise of the demodulated grey) vs the chain's linear prediction + clicks, 14–23 dB | 4 standard errors of a variance of ~2.6e5 independent samples + the linearisation's 1/(2 CNR) | none |
| `--threshold` slope | least-squares dB/dB over the four points | the worst point's tolerance through the fit, Σ\|x − x̄\| / Σ(x − x̄)²: 0.035 | none |
| `--threshold` knee | CNR where noise is 1 dB over the linear law, half-dB sweep, vs Rice + Campbell | the knee's shift for a factor 1.5 in click energy (a click is a swing, not an impulse) + for the 1/(2 CNR) term + a quarter of the grid: 0.59 dB (disc), 1.11 (TE) | none |
| `--threshold` extension | knee(disc) − knee(TE) vs the model's | the two knees' tolerances summed | none |
| `--threshold` collapse | SNR lost from the knee to 3 dB under it | more than 3 dB (the linear law's), by definition | none |
| `--dispersal` | every output pixel of a flat grey vs the triangle through each clamp, bilinear exactly as the output reads | float (9 passes × 128 terms × 2⁻²⁴ / 0.7 = 1.1e-4) + the de-emphasis's 0.41 µs group delay on the along-row slope; Slow adds a row's worth of porch slope (the recursion against continuous time) and its 576-row truncation | **yes**: read in the output raster. Bilinear weights may be quantised to 1/256 on some GPUs; between neighbouring rows the triangle moves 1.2e-3, so that costs 5e-6, inside the float term |
| `--dispersal` good bound | Clamp Good's residual | slope × the farthest active sample from the porch centre (53.8 µs) + tol | yes, as above |
| `--identity` | four flat quadrants, Component, noiseless, 8-bit | one code: float (1e-4) and the 8-bit rounding (half a code) can move a pixel one code, no further; margin 163 video samples from any edge | yes: at 320×180 the interior is 28%; asserted ≥ 5% so the check cannot lose its subject |
| `--seed` | ( seed, field ) → bit-identical video; other seed or field → different; 320×180 and 1280×720 → bit-identical video | exact | bit-identity is per GPU; across rasters it holds anywhere, since the link never sees the raster |
| `--resize` | frame after 1× → 2× → 1× equals a fresh instance's | exact | none: no state between frames |
| `--clock` | rows at 7.049 s and six days later | 2 float ULPs of 1 MHz; measured 0 | none: CPU, double |
| `--rice` | the integrator vs `r erfc( √ρ )` at δf = 0; the sampled rate's shortfall closing from below at 8×, 16×, 32× | 1e-6 relative; monotone | none |
| `sweep.py` | each control changes ≥ 1 subpixel | any change | run at 320×180 locally; not in CI (too slow on the software renderer) |

Two things are deliberately not relied on. Exact cancellation: no check asserts `== 0`
on a difference of computed floats except the seed and resize checks, which compare
two runs of the same operations. And `pow` returning exact values: the harness reads
the effective CNR, taps and deviation back from the plugin (`ResolvedForTest`) rather
than trusting the slider it set.

What might still differ on another rasteriser: a software GL that cannot create a 4.1
core context at all. CI runs `--offline` and compiles the shaders through glslc. The
GitHub runner does get Apple's software renderer, but the link is run per sample on the
GPU and the software renderer is roughly a hundred times slower: measured with
`DLTEST_RENDERER=software`, `--emphasis` takes ~25 s and `--dispersal` ~4 min at
320×180, and every other rendered check more than four minutes, so the full set ran past
a 40-minute timeout on every push. CI therefore renders only those two; the rest, and the
sweep, are the dev Mac's (`tools/verify.sh`) and the Windows build is swept in Arena.

---

## Negative controls and the mutation

`dltest --negative` perturbs the **model** through a test hook (always off in the
plugin) or, for the two offline checks, perturbs what the check is given, and runs the
check unchanged. Every case must fail, and must fail the named check:

| perturbation | check that must fail | failed |
| --- | --- | --- |
| the host clock taken in float | `clock` | 1 of 1 |
| the pre-emphasis pole 5% off | `emphasis F.405 pre` | 1 of 2 |
| the discriminator linearised, `ψ = Im m` unwrapped (no clicks: the spec's) | `clicks rate` | 4 of 4 |
| the noise rotated the wrong way into the carrier's frame | `polarity` | 6 of 6 |
| the Slow clamp's time constant skipped (the spec's) | `dispersal slow` | 1 of 1 |
| the de-emphasis pole 5% off | `emphasis flat` | 1 of 3 |
| a receiver that believes the deviation 2% higher | `identity` | 1 of 2 |
| a receiver that believes the deviation 10% higher | `threshold offset` | 4 of 5 |

**The GLSL mutation.** On a committed, clean tree (at `a41f10a`), the
received phasor `vec2( 1.0 + m.x, m.y )` in the link library was changed to
`vec2( 1.0 - m.x, m.y )`, one character. That reflects the noise, which leaves every
statistic of an unmodulated carrier alone and reverses the modulation term. Exactly
one check caught it, `--polarity`, 6 of 40 at both rasters (+/− = 3.51 against 0.268
at 6 dB); `--clicks`, `--threshold`, `--identity` and the rest passed, as they should
have. It was reverted with `git checkout` and the checks re-run green. That proves the
harness drives the shader text the plugin ships (the probe runs the same library), and
that `--polarity` is the only check that can see which way the noise turns.

---

## Decisions taken without asking

- **The picture is the field on air, drawn at 576 rows.** Rows are half a line apart
  in time; a row is still a whole 64 µs line horizontally (porch, active, porch). The
  alternative, both fields interlaced into one frame, combs a progressive clip.
- **The dispersal is locked to the frame**, even fields ramping up. Uplinks locked it.
- **The IF is Gaussian**, `|H|² = exp( −f² / 2σ² )`, −3 dB width B. Rice's rms
  bandwidth is then σ exactly, its impulse response is short, and it is a fair
  stand-in for a SAW filter. CNR is carrier over all the noise in that IF.
- **The discriminator runs at 16× the video rate** (see above), and each link sample
  is the sum of the 8 fine outputs centred on it. Off centre by half a link sample
  would delay chroma 14 ns against the decoder's reference and turn hues 22°.
- **The uplink interpolates between link samples with Catmull-Rom** (−0.06 dB at
  5 MHz; linear interpolation loses 0.6 dB).
- **Both emphasis networks are matched-z**, not bilinear: bilinear misses F.405's
  tolerance at 5 MHz (0.18 dB against 0.15), prewarped or not.
- **The carrier rests at 0.2 V**, the middle of a 1 V composite. With F.405's −11 dB at
  DC, white sits only 1.9 MHz above centre.
- **Component is three FM links**, Y, U and V, each with its own noise and its own
  dispersal (an idealised MAC: MAC time-compressed them onto one carrier). U and V
  rest at 0 V.
- **Threshold Ext. is an idealised tracking filter**: a filter that follows the carrier
  perfectly, 3 dB narrower in noise bandwidth, so its noise is drawn in the carrier's
  frame with no rotation and 3 dB less power. It therefore has no modulation term: the
  IF is assumed flat across the tracking filter's excursion. The knee moves 3.9 dB,
  not 3, because the narrower noise also halves Rice's r and trims the grain at the
  band edge; `--threshold` checks the 3.9 against the model.
- **Clamp Good is a keyed back-porch clamp**: the mean of 32 samples (1.8 µs) of this
  row's porch. A click on the porch moves the whole line, which is what keyed clamps
  did. No colour burst is modelled; the decoder's reference is perfect.
- **Clamp Slow** is `c[y] = c[y−1] + α ( p[y] − c[y−1] )`, τ = 2 ms, written as its
  sum so every row is its own fragment. Its 576 rows of history before row 0 come from
  the row table in closed form, noise-free: the vertical interval carries no picture,
  and a history remembered from the last frame would be a frame from another field.
- **Rain Fade and Audio Fade subtract dB from CNR.** Audio Fade is up to 12 dB at an
  RMS bin level of 1, instantaneous; nothing reads the bins' layout.
- **The noise is redrawn per field**, keyed on the field index's low 32 bits.
- **Output**: the decoded straight colour is clamped, multiplied by the clip's alpha
  and mixed; alpha is the clip's.
- **The shaders are dumped, not scraped**, for glslc: the link and the probe are one
  macro and two mains, which the fleet's regex extraction cannot expand.
- **No factory presets**, like pitch and slowscan. The user guide is `docs/USER-GUIDE.md`.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated** by stoatworks-backend's
  `sync-about.py` and `sync-attributions.py` from the website's projects.json and the
  attribution master lists. Edit those, not these files; the next sync overwrites them.
- **The commit trailer names the model that wrote this** (`Claude Opus 5.5`), as the
  session's instructions required, not the brief's `Fable 5.1`.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4 (2026-09-23)

Every number below is `tools/verify.sh` on this machine against a fresh universal
Release build, at 320×180 and 1280×720 (the link's numbers are identical at both).

- **F.405.** Pre-emphasis within 0.0011 dB of the recommendation's curve, de-emphasis
  within 0.0188 dB (tolerance 0.10–0.15). Pre × de flat to 0.0181 dB across 0.05–5.05
  MHz (bound 0.0209).
- **Clicks.** 9,115 / 3,407 / 2,031 / 257 at 4 / 6 / 8 / 10 dB against the sampled
  discriminator's exact 8,983 / 3,378 / 1,975 / 257; Rice at 64×: 286,302 /s against
  286,665.
- **Polarity.** Near white (carrier 1.902 MHz off centre): +/− = 0.279 against 0.268
  at 6 dB, 0.198 against 0.200 at 8 dB. The textbook's large-ρ formula would say
  0.435 and 0.387; it is not the right formula this close to threshold.
- **Threshold.** SNR − CNR = 20.59 dB predicted, measured within 0.084 dB at 14 dB and
  0.016 at 23 dB; 1.0074 dB/dB (Threshold Ext.: 21.30 dB, 1.0033). Knee at 8.71 dB
  against 8.57 predicted; Threshold Ext. at 4.87 against 4.67; it moves 3.84 dB
  against 3.90. From the knee to 3 dB under it the SNR falls 9.7 dB.
- **Dispersal.** Clamp Off: every pixel within 1.8e-5 of the triangle (tolerance
  1.1e-4), 0.739 of luma swing across two fields; Slow within 6.0e-4 (tolerance
  1.35e-3), a ±0.076 lag against slope × τ = 0.075; Good within 1.4e-6, residual
  1.7e-3 under its 2.1e-3 per-line bound.
- **Identity.** Every interior pixel of four saturated quadrants back to the same 8-bit
  code (worst 0).
- **Seed, resize, clock.** Bit-identical where they should be; different where they
  should be.
- **Negative controls.** All eight fail as they should. **Mutation.** Caught by
  `--polarity`, reverted.
- **No dead controls.** All 14 sweepable parameters, at 320×180.
- **Every shader compiles** through glslc (12).
- **The bundle** is universal, exports `_plugMain`, carries
  `com.stoatworks.ffgl.downlink`, ad-hoc signs, and `oxbow` reports `SW Downlink` /
  `DL01` / `effect` and renders 120 frames through `plugMain`.
- **Cost**, best of three runs of 30 frames after a warm-up, `glFinish` both sides, on a
  GPU shared with other builds: PAL **3.1 ms** at 720p, **3.2 ms** at 1080p, **3.7 ms**
  at 4K; Component **5.8 / 5.9 / 6.5 ms**. The link raster is fixed, so the output size
  hardly matters. By timer query at 1080p the link is 1.5 ms (PAL) and 4.2 ms
  (Component), the de-emphasis 0.36, the pre-emphasis 0.36. The run before it, minutes
  earlier on the same shared machine, read Component 8.2 ms at 1080p.

### Assumed, or not done

- ☠️ **Never loaded into Resolume on macOS.** On Windows, v0.1.0's CI build passed
  the fleet Arena gate 9 of 9 in Arena 7.27.1 on llvmpipe (2026-09-24): it loads,
  registers as `SW Downlink` / `DL01` / effect, all 21 host controls match the
  declaration, it renders and the log is clean. 11 controls moved the picture (2 under
  a precondition); Deviation, Dispersal and IF Bandwidth read inconclusive, because the
  link's noise changes every frame; Audio and Audio Fade were not testable, because
  win-lab has no sound device. On macOS the inspector presentation is untested, and on
  either platform the audio input's routing, the host's clock and its FFT bins are.
- **The coloured grain** (the FM noise triangle landing on the chroma subcarrier) falls
  out of the chain and is visible, but no check measures the noise spectrum's shape;
  `--threshold` checks its total.
- **Clicks cluster** below threshold (index of dispersion 1.19 at 4 dB), so the
  Poisson interval the spec asks for is slightly optimistic there.
- **3 ms of GPU for PAL and 6 for Component** may be too much beside other effects on
  a show machine. The link is the cost; halving the discriminator's rate would quarter
  it and lose 13–36% of the clicks (see `--rice`).
- **Windows has only met a software renderer.** CI builds it on GitHub, and Arena on
  win-lab's llvmpipe is the one host it has run in; that says nothing about a GPU or
  about speed.
- **Resolume's 64 bins are unmeasured**, as fleet-wide.
- **No OpenFX port.** Not required for 0.1.0. The browser demo exists; see below for
  what it is not.
- **Nothing has been through a show.**

---

## The browser demo

`demo/` is the page at **downlink-demo.stoatworks-labs.com**. Two halves, not equally
faithful:

- **The GPU passes are the plugin's.** `demo/plugin.js` carries every shader the plugin
  renders with — the vertex, resample, encode, preemph, detect, deemph, porch, clamp,
  decode, output, and the link as `LINK_LIBRARY` (the `DOWNLINK_LINK_LIBRARY` macro) plus
  `LINK_MAIN`, joined as C++ joins adjacent literals. `demo/tools/check_shaders.py`
  compares all twelve pieces character for character, and the join, and `tools/verify.sh`
  runs it. `tools/check-shaders.sh` is a different check: it puts the plugin's own
  shaders (from `dltest --dump-shaders`) through glslc and never looks at the page. The
  link probe is not carried: only `dltest` runs it.
- **The CPU half is a port, and nothing checks it but a reader.** `Link.cpp` (both
  emphasis networks, the lowpass, the IF's taps, the triangle, the row table, the field
  index, the subcarrier phase), the conversions in `Controls.h`, and `Downlink.cpp`'s
  `resolve()`, clock voting and every uniform `ProcessOpenGL` sets are ported to JS
  function for function, in double, with `Math.fround` wherever the C++ is float. It was
  cross-checked once, by hand, on 2026-09-24: a throwaway program printing Link.cpp's and
  Controls.h's outputs for a spread of inputs (taps at several bandwidths, row tables at
  five host times up to 499,217 s, the phases, the conversions) against the port — 12,327
  numbers, the worst 7.7e-16 relative. That is not a standing check. Change any of those
  files and change the page by hand.

What the page does not do, all said on it (banner, the disclosure, the line under the
canvas):

- **No audio.** Audio Fade is on the panel because the plugin declares it, labelled
  `no audio`; the level is 0, so it changes nothing (measured: 0 pixels differ at full
  Audio Fade). The `Audio` FFT buffer parameter is not on the panel: nobody sets it by
  hand and the kit has no buffer control.
- **Noise Seed is a dropdown** of all 1000 values; the kit has no integer control.
- **The About block is absent**, as on every page. The test hooks are absent; their inert
  values are set, as the plugin sets them.
- **The clock is the page's**, in seconds; the unit vote is ported and settles on seconds
  after four frames.
- **Every signal buffer is RGBA32F** (`EXT_color_buffer_float`, as in the plugin). The
  decoded picture, filtered by the output pass, is RGBA32F where the browser has
  `OES_texture_float_linear` and RGBA16F where it does not; the page says which.

Decided without asking: **the link runs at its full raster** (2002 × 576, the
discriminator at 16×), because the clicks the plugin is for depend on it; a weak GPU is
slow rather than wrong. It ran at 60 fps on an M4 Max through ANGLE/Metal, and rendered
correctly through SwiftShader. **Colour bars first** in the clip list. **The presets are
the page's own**, named as such (the plugin ships none), and the two flicker presets say
so in their names.

---

## Open questions

- **Should the default be Clamp Slow?** Good is the correct receiver and the default;
  Slow is the look people remember from cheap receivers. An operator who wants the
  rocking has to find the Clamp control.
- **Should the noise follow the host's frames instead of the fields?** At 60 fps the
  noise repeats on one frame in six. That is what a 50 Hz receiver on a 60 Hz display
  does, but it may read as a stutter.
- **Is 16× right?** 32× would miss 1–3% of the clicks instead of 4–11% and cost about
  four times the link.
- **A real burst and a real PLL?** The decoder's reference is perfect and the
  threshold extension is an ideal tracking filter. Both could be modelled, and both
  are sequential along the line, which this GPU design avoids.

---

## Siblings

- **old-cathode**: the PAL encoder and decoder, the 4fsc raster, the resample.
- **ferric**, **compander**: one-dimensional processing along the row.
- **readout**, **pitch**: the host clock-unit voting; the per-row table in double.
- **slowscan**: the other FM discriminator in the fleet, on the CPU, and its knee.
- **tinsel**, **pitch**: `PassBuffer`, the sweep, `verify.sh`, CI.
- **oxbow**: `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
