# Attributions

Downlink is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is generated — the master lists live in the `stoatworks-backend` repo and are
pushed out by `scripts/sync-attributions.py`. Edit it there, not here.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### PAL encoder and decoder — Stoatworks old-cathode

<https://github.com/stoatworks-labs/old-cathode>  
Licence: MIT  
Copyright: Stoatworks Labs

The PAL encoder and decoder, the 4fsc raster and its three-tap chroma notch, and the resample pass are old-cathode's, adapted to read a received composite from a texture.

### Host clock-unit voting — Stoatworks readout

<https://github.com/stoatworks-labs/readout>  
Licence: MIT  
Copyright: Stoatworks Labs

The host clock-unit voting is readout's; the per-row table computed in double and handed over frame-relative is pitch's shape.

### PassBuffer, sweep, verify and CI shape — Stoatworks tinsel

<https://github.com/stoatworks-labs/tinsel>  
Licence: MIT  
Copyright: Stoatworks Labs

PassBuffer, tools/sweep.py, tools/verify.sh, the --pipe format and the CI shape are tinsel's by way of pitch and slowscan. The --offline split for a runner with no GL is spasis's lesson.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl (third_party/ffgl in oxbow).

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Standards and published specifications

What the implementation is measured against.

- **ITU-R F.405-1, pre-emphasis characteristics for FM radio-relay systems for television ("CCIR 405")** — The 625-line curve 10 log[(1 + 10.21 f^2)/(1 + 0.4083 f^2)] - 11 dB and its tolerance are its Table 1 and recommends 4. Both networks in source/Link.cpp are matched-z versions of it, and dltest --emphasis checks them against its curve and tolerance.
- **S. O. Rice, "Noise in FM receivers", in Time Series Analysis, M. Rosenblatt (ed.), Wiley, 1963; and Taub & Schilling, Principles of Communication Systems** — The click rate r erfc(sqrt(rho)) and its modulation term, and the method behind dltest's exact rates: the rate at which a Gaussian phasor crosses the negative real axis, each way. The threshold is the textbook's definition; the click noise is summed by Campbell's theorem.
- **The Rician phase distribution (Proakis, Digital Communications; Middleton)** — The density of the phase of a constant plus circular Gaussian noise, in closed form; dltest integrates it to get the sampled discriminator's exact slip rate.
- **PAL and ITU-R BT.601** — The PAL-I subcarrier, 283.7516 cycles a line, and the BT.601 Y, U, V weights, as used by old-cathode.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
