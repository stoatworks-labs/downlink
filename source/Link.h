#pragma once

#include <cstdint>
#include <vector>

/**
	The satellite link's numbers, and the filters built from them.

	No GL here, on purpose: the harness designs the same filters, reads the
	same constants and makes the same per-row table to predict what the GPU
	should do, and the offline checks (`dltest --offline`) run where there is
	no GL context at all.

	**Three rasters, all tied to one clock.** The video is PAL sampled at four
	times the colour subcarrier, `kFsVideo` (17.734475 MHz): 921 active samples
	a line, old-cathode's raster, where the decoder's chroma notch is a plain
	average. The uplink and the downlink's detector run at twice that,
	`kFsLink`, 2002 samples a line with the porches. And the discriminator
	itself runs at `kOversample` times the video rate, 283.75 MHz, eight fine
	samples per link sample, fine enough that its 2 pi slips are the ones
	Rice counted in continuous time (see AGENTS.md, "Why sixteen").

	**One row is one line.** Each row carries a back porch (the clamp's
	reference), the active line and a front porch, so a receiver clamp has
	something to measure. Rows are scanned at `kRowSeconds` apart within the
	field that is on air; see `RowTable`.
*/
namespace downlink::link
{
//--- the video raster ----------------------------------------------------
constexpr double kFsc      = 4.43361875e6;//PAL colour subcarrier, Hz
constexpr double kFsVideo  = 4.0 * kFsc;  //4fsc: 17.734475 MHz
constexpr int kLinkFactor  = 2;           //link samples per video sample
constexpr double kFsLink   = kLinkFactor * kFsVideo;
constexpr int kOversample  = 16;          //discriminator samples per video sample
constexpr int kSubPerLink  = kOversample / kLinkFactor;//8
constexpr double kFsFine   = kOversample * kFsVideo;//283.75 MHz

constexpr int kActive      = 921;//samples of active line at 4fsc (51.95 us)
constexpr int kBackPorch   = 64; //3.61 us at blanking, before the active line
constexpr int kFrontPorch  = 16;
constexpr int kLine        = kBackPorch + kActive + kFrontPorch;//1001 video samples
constexpr int kLinkLine    = kLine * kLinkFactor;             //2002 link samples
constexpr int kLinkActive  = kActive * kLinkFactor;           //1842
constexpr int kRows        = 576;

/// The clamp's window on the back porch, in video samples. Clear of the line
/// start by more than the detector lowpass's half-length, and clear of the
/// active line by more than it too (see `--dispersal`).
constexpr int kClampFirst  = 16;
constexpr int kClampCount  = 32;

//--- time ----------------------------------------------------------------
constexpr double kFieldSeconds    = 0.02;    //625/50
constexpr double kLineSeconds     = 64e-6;
/// The clip is progressive and the field carries half its lines, so the
/// picture is drawn as the field on air with both of the clip's rows for each
/// field line: consecutive rows are half a line apart in time.
constexpr double kRowSeconds      = kLineSeconds / 2.0;
/// The first active line of a field (line 23) starts 22 lines in.
constexpr double kFirstRowSeconds = 22.0 * kLineSeconds;
constexpr double kDispersalHz     = 25.0;    //triangle, locked to the frame

//--- levels --------------------------------------------------------------
constexpr double kWhiteVolts = 0.7;//blanking 0, white 0.7 V
/// The carrier's rest frequency is the midpoint of the 1 V composite, 0.2 V
/// above blanking (sync tip at -0.3 V, peak white at +0.7 V).
constexpr double kRestVolts  = 0.2;

//--- ITU-R F.405-1, 625 lines --------------------------------------------
/// relative deviation (dB) = 10 log[ (1 + C f^2) / (1 + B f^2) ] - A, f in MHz.
constexpr double kPreA = 11.0;
constexpr double kPreB = 0.4083;
constexpr double kPreC = 10.21;

/// Threshold extension's stated improvement: the tracking filter narrows the
/// noise reaching the discriminator by this much.
constexpr double kThresholdExtensionDb = 3.0;

/// The Slow clamp's time constant: a DC restorer on a capacitor.
constexpr double kSlowClampSeconds = 2e-3;

//--- limits the shaders are compiled for ---------------------------------
constexpr int kMaxPreTaps  = 64; //causal, at kFsLink
constexpr int kMaxDeTaps   = 128;//causal, at kFsVideo
constexpr int kLpfHalf     = 32; //symmetric, at kFsLink
constexpr int kMaxNoiseHalf = 47;//symmetric, at the fine rate
/// The link shader's window arrays: P Sub + 1 + 2 NoiseHalf fine samples.
constexpr int kLinkWindow   = 128;
/// History rows before row 0 the Slow clamp integrates over (analytic: the
/// vertical interval and the tail of the previous field carry no picture).
constexpr int kHistoryRows = 576;

//--- F.405 and the digital networks ---------------------------------------
/// The recommendation's own curve, in dB, at f MHz.
double F405Db( double fMHz );
/// Its tolerance on a practical network: +/-( 0.1 + 0.05 f / fc ) dB.
double F405ToleranceDb( double fMHz, double fcMHz );
/// Pre-emphasis's gain at a low frequency, 10^( -A / 20 ).
double PreDcGain();

/// Pre-emphasis at the link rate, matched-z, DC gain PreDcGain(). Causal.
std::vector< double > PreEmphasisTaps( double fs = kFsLink );
/// De-emphasis at the video rate: the matched-z inverse. Causal.
std::vector< double > DeEmphasisTaps( double fs = kFsVideo );
/// Hann-windowed sinc lowpass at the link rate, 2 kLpfHalf + 1 taps, DC 1.
std::vector< double > VideoLowpassTaps( double cutoffHz, double fs = kFsLink );

/// A matched-z first-order network's exact response, for the harness.
struct FirstOrder
{
	double gain, zero, pole;//H(z) = gain ( 1 - zero z^-1 ) / ( 1 - pole z^-1 )
};
FirstOrder PreEmphasisNetwork( double fs = kFsLink );
FirstOrder DeEmphasisNetwork( double fs = kFsVideo );

//--- the IF ----------------------------------------------------------------
/// The IF is Gaussian: |H(f)|^2 = exp( -f^2 / 2 sigma^2 ), -3 dB width B, so
/// sigma = B / ( 2 sqrt( 2 ln 2 ) ). sigma IS Rice's rms bandwidth r.
double IfSigmaHz( double bandwidthHz );
/// Symmetric amplitude taps at the fine rate, sum of squares 1, half-length
/// ceil( 4 sigma_t ). Index i is lag ( i - half ).
std::vector< double > NoiseTaps( double sigmaHz, double fsFine = kFsFine );

//--- the dispersal and the rows -------------------------------------------
/// The 25 Hz triangle at time t (s) in the frame cycle, MHz: -pp/2 at the
/// start of an even field, +pp/2 at its end, back down over the odd one.
double Triangle( double cycleSeconds, double ppMHz );

/// Everything per row the shaders need, computed in double from the host's
/// clock and handed over frame-relative. Row y of the table is scan row
/// ( y - kHistoryRows ): the first kHistoryRows entries are the history the
/// Slow clamp integrates over.
struct RowTable
{
	int64_t field = 0;         ///< the field on air
	int parity    = 0;         ///< field & 1
	std::vector< float > base; ///< dispersal at the row's first video sample, MHz
	std::vector< float > slope;///< MHz per video sample along the row
	std::vector< double > cycleSeconds;///< each row's start in the 40 ms cycle
};
void MakeRowTable( double hostSeconds, double dispersalPpMHz, RowTable& table );
/// The field index for a host time, in double. Kept separate so the clock
/// check can compare a fresh clock with a six-day one.
int64_t FieldIndex( double hostSeconds );

//--- composite phase -------------------------------------------------------
/// Subcarrier radians per link sample, per row and per field, the per-field
/// one reduced in double (the field count is huge on a real host).
double SubcarrierStepPerLinkSample();
double SubcarrierStepPerRow();
double SubcarrierPhaseForField( int64_t field );

} // namespace downlink::link
