# Attributions

Downlink is built on other people's work. This file lists what that work is, who did
it, and what it is doing here.

It is PROVISIONAL: hand-written in the shape the `stoatworks-backend` sync
(`scripts/sync-attributions.py`) generates. Downlink is not yet registered in that
script's lists, so the sync cannot produce this file yet. Once the registration is
finished the sync overwrites this file; edit it there, not here.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl, pinned to b1afaf9.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives through the vcpkg manifest on Windows only. Not fetched on macOS.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something this plugin calls — listed because it is present in the checkout. The harness writes PNGs through the system zlib and not through this.

## Standards and published work this project implements

Not code, and nothing was copied from anyone's source, but the numbers and the
theory are other people's.

### ITU-R F.405-1, pre-emphasis for FM television

*Pre-emphasis characteristics for frequency modulation radio-relay systems for
television* (1959–1963–1970; editorial revision 2001), the recommendation the
satellite links inherited as "CCIR 405". The 625-line curve
`10 log[ (1 + 10.21 f²) / (1 + 0.4083 f²) ] − 11 dB` and its tolerance
`±(0.1 + 0.05 f / fc) dB` are its Table 1 and recommends 4. Both networks in
`source/Link.cpp` are matched-z versions of it, and `dltest --emphasis` checks them
against its own curve and tolerance.

### Rice's click theory of the FM threshold

S. O. Rice, "Noise in FM receivers", in *Time Series Analysis*, M. Rosenblatt (ed.),
Wiley, 1963; and the textbook treatment in Taub & Schilling, *Principles of
Communication Systems*.

The click rate `r erfc( sqrt( ρ ) )` and its modulation term, and the method behind
`dltest`'s exact rates: the rate at which a Gaussian phasor crosses the negative real
axis, each way. The threshold is stated by the textbook's definition (the output
noise 1 dB over the above-threshold formula); the click noise is summed by Campbell's
theorem.

### The Rician phase distribution

The density of the phase of a constant plus circular Gaussian noise, in closed form
(as in Proakis, *Digital Communications*, and Middleton before it). `dltest` integrates
it to get the sampled discriminator's exact slip rate.

### PAL and BT.601

The PAL-I subcarrier, 283.7516 cycles a line, and the BT.601 Y, U, V weights, as used
by old-cathode (below).

## Within the fleet

Not third-party, but owed a line. The PAL encoder and decoder, the 4fsc raster and
its three-tap chroma notch, and the resample pass are **old-cathode**'s
(`github.com/stoatworks-labs/old-cathode`, MIT, Stoatworks Labs), adapted to read a
received composite from a texture. The host clock-unit voting is **readout**'s. The
per-row table computed in double and handed over frame-relative is **pitch**'s shape.
`PassBuffer`, `sweep.py`, `verify.sh`, the `--pipe` format and the CI shape are
**tinsel**'s by way of **pitch** and **slowscan**. The `--offline` split for a
runner with no GL is **spasis**'s lesson.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
