# downlink

Analogue FM satellite television, as an FFGL **effect** for Resolume Arena/Avenue.
C++/GLSL, CMake MODULE → universal `.bundle` (macOS) + Windows `.dll`. MIT.

Read `AGENTS.md` before changing the link library in `source/Shaders.cpp`, anything
in `source/Link.*`, or any check's tolerance.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build`. **Do not run this from a session.** It
  writes into `~/Documents/Resolume Arena/Extra Effects`.
- Render a frame offline: `./build/dltest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "CNR=0.3" --set "Clamp=0" --set "Composite=1"`
  (0..1 for sliders, the element index for options, the real value for Noise Seed)
- Other sources: `--source flat --level 128`, `--source quadrants`. A synthetic
  spectrum for Audio Fade: `--audio 1`
- List parameters, kinds, defaults and ranges: `./build/dltest --list`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, an optional `--script` of timed `frame Name Value` cues and
  `--audio L`:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/dltest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`
  A cue line is `frame  Parameter Name  value` (`#` starts a comment), in the same
  units as `--set`; `@audio` as the name cues the synthetic spectrum's level. Values
  interpolate linearly between a name's cues and hold before the first and after the
  last. Frame *n* is clocked at `n / 60` s. An unknown name exits 2 before any frame;
  a partial frame at EOF ends the stream with exit 0.
- Every shader the plugin compiles, as files: `./build/dltest --dump-shaders DIR`

## Verify
- Everything: `tools/verify.sh` (a fresh universal build, the shaders through glslc,
  the offline checks, every GL check at 320x180 AND 1280x720, the pipe, the sweep,
  the bench, the bundle; about three minutes on a quiet machine)
- The shaders alone: `tools/check-shaders.sh build/dltest` (needs `brew install shaderc`)
- F.405 met; pre- then de-emphasis flat: `./build/dltest --emphasis`
- Click counts against the sampled discriminator's exact rate, and Rice at 64x:
  `./build/dltest --clicks`
- More negative clicks near white: `./build/dltest --polarity`
- SNR = CNR + the FM improvement, the knee, threshold extension: `./build/dltest --threshold`
- The dispersal through each clamp, per pixel: `./build/dltest --dispersal`
- A clean link returns the input: `./build/dltest --identity`
- The noise is ( seed, field ) only: `./build/dltest --seed`
- A resize mid-run leaves nothing: `./build/dltest --resize`
- The checks can fail: `./build/dltest --negative`
- Everything with no GL (what CI runs first): `./build/dltest --offline`
  (`--names --clock --emphasis-offline --rice --negative-offline`)
- CI's GL step: add `--allow-no-gl`, which SKIPs loudly when there is no context
- Quieter: `--quiet` prints failures and the summary only
- No dead controls: `python3 tools/sweep.py --binary build/dltest` (`--size WxH`, `--jobs N`)
- Render cost and per-pass GPU time: `./build/dltest --bench` (`--bench-frames N`)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Downlink.bundle`

## Notes
- **The link is GPU code, per link sample, in parallel.** The link library
  (`DOWNLINK_LINK_LIBRARY` in `source/Shaders.cpp`) draws the noise in the carrier's own frame from local phase
  DIFFERENCES over the IF filter's half-length. Nothing is integrated along a line.
  Read the comment at its top before touching it.
- **The probe runs the plugin's library.** `kLinkShader` and `kLinkProbeShader` are one
  macro plus two mains; a change to the link changes both.
- **Scan row y is texel row y** in every signal buffer: row 0, the first line, is the
  TOP of the picture. Only `resample` and `output` flip.
- **Nothing absolute crosses from the host clock.** The field index is taken in double
  and only its parity and its low 32 bits (the noise key) reach GLSL; the dispersal is
  a per-row table of small numbers. Resolume's clock has been seen at 499,217 s.
- **Test hooks are always off in the plugin**: `Set*ForTest` on `Downlink`. They exist
  so `--negative` can break the model, not the check.
- **The Slow clamp's history is computed, not remembered**: 576 rows before row 0 come
  from the row table in closed form. The plugin keeps no picture between frames.
- **Audio Fade reads the total level only** (RMS of the bins): nobody has measured
  Resolume's 64 bins.
- **Parameter names must be unique**: `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1, so every slider is 0..1 and
  `Controls.h` holds the units. Options are mapped by index.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `downlink_core` is an OBJECT library, not STATIC: the plugin registers itself from a
  file-scope constructor nothing references by name.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `DL01`, display name `SW Downlink`.

## Not done yet
- **Never loaded into Resolume.** Everything numeric is measured offline on macOS,
  plus an `oxbow` load. The Windows build is CI-only and has never run.
- No user guide, no OpenFX port, no browser demo, no factory presets.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies with `guide=""`.

## Diagnostics

`source/Diag.{h,cpp}`: a log file only, no crash handler (this runs inside Resolume).
A shader that will not compile is logged with the compiler's own message.

    ~/Library/Logs/downlink/downlink.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\downlink\logs\downlink.YYYY-MM-DD.log   (Windows)
