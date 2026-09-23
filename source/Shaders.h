#pragma once

/**
	The passes, in the order they run. Every signal buffer stores scan row y
	at texel row y: row 0, the first line scanned, is the TOP of the picture.
	Only `resample` (reading the clip) and `output` (writing the host) flip.

	1. **resample** -- the clip down to the link raster's active line,
	   1842 x 576, box-filtered over each destination sample so nothing above
	   the line's Nyquist is aliased into the subcarrier (old-cathode's
	   reason). Straight colour.

	2. **encode** -- the uplink's baseband: PAL composite (old-cathode's
	   encoder, at 8 fsc) or three component signals, in volts, with the back
	   and front porches at blanking, less the carrier's rest level.
	   2002 x 576.

	3. **preemph** -- the CCIR 405 pre-emphasis network, causal.

	4. **link** -- the FM carrier, the channel and the discriminator, per link
	   sample, in parallel. See the comment at the top of `kLinkLibrary`: the
	   discriminator output at fine sample k is arg( r[k] conj r[k-1] ), and it
	   needs nothing but the local phase increment and noise drawn in a local
	   frame, so no phase is ever integrated along a line. 2002 x 576, in
	   volts of pre-emphasised video.

	5. **detect** -- the video lowpass at the link rate, and the decimation to
	   the video rate. 1001 x 576.

	6. **deemph** -- the de-emphasis network, and the rest level put back.

	7. **porch** -- the demodulated back porch of each row, 1 x 576.

	8. **clamp** -- what the receiver's clamp takes off each row: nothing, a
	   leaky average of the porches down the field, or this row's porch.

	9. **decode** -- the clamp applied, and old-cathode's PAL decoder (or the
	   component matrix) back to RGB. 921 x 576.

	10. **output** -- the host's raster, and the mix.

	`kLinkProbeShader` is the harness's: the same library as `kLinkShader`,
	with a main that writes each fine sample's discriminator output instead of
	summing them, so clicks are counted in the discriminator's own output.
	Both are assembled at compile time from one macro, so they cannot drift.
*/
namespace downlink::shaders
{
extern const char* const kVertexShader;
extern const char* const kResampleShader;
extern const char* const kEncodeShader;
extern const char* const kPreemphShader;
extern const char* const kLinkShader;
extern const char* const kLinkProbeShader;
extern const char* const kDetectShader;
extern const char* const kDeemphShader;
extern const char* const kPorchShader;
extern const char* const kClampShader;
extern const char* const kDecodeShader;
extern const char* const kOutputShader;

struct Named
{
	const char* name;
	const char* text;
};
/// Every shader the plugin compiles, for `dltest --dump-shaders`.
extern const Named kAll[];
extern const int kAllCount;
} // namespace downlink::shaders
