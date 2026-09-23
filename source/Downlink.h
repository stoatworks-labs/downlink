#pragma once

#include "Link.h"
#include "PassBuffer.h"

#include <FFGLSDK.h>

#include "StoatworksAboutParams.h"

#include <string>
#include <vector>

/**
	Downlink -- analogue FM satellite television, as an FFGL effect.

	**The one idea.** Satellite TV sent composite video as frequency
	modulation of a carrier, because FM trades bandwidth for noise, and that
	trade has a cliff. Build the link -- PAL, CCIR 405 pre-emphasis, the FM
	carrier with its energy dispersal, Gaussian noise in a Gaussian IF, an
	arg-difference discriminator, de-emphasis, a video lowpass, a clamp, the
	PAL decoder -- and the look falls out of the discriminator's maths:

	  - above threshold, fine coloured grain, the FM noise triangle landing
	    its worst on the chroma subcarrier;
	  - below it, clicks: the received phasor swung round the origin, a 2 pi
	    slip, smeared by the de-emphasis and the lowpass into the white and
	    black "sparklies", at Rice's rate, more black ones on white;
	  - with a bad clamp, the 25 Hz dispersal triangle rocking the brightness.

	Nine passes, in `Shaders.h`. The per-row dispersal is computed in double
	here and handed over frame-relative; see `Link.h` for every number.
*/
class Downlink : public CFFGLPlugin
{
public:
	Downlink();

	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;
	/// The About line must accept its own default, or no host can
	/// instantiate the plugin: CFFGLPlugin's version fails, and a failed
	/// default deletes the instance.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	enum ParamID : FFUInt32
	{
		//Uplink
		DL_DEVIATION,
		DL_DISPERSAL,
		DL_PRE_EMPHASIS,

		//Link
		DL_CNR,
		DL_RAIN,
		DL_AUDIO_FADE,
		DL_AUDIO,//the host's FFT
		DL_IF_BANDWIDTH,
		DL_SEED,

		//Receiver
		DL_DEMODULATOR,
		DL_CLAMP,
		DL_DE_EMPHASIS,
		DL_VIDEO_BANDWIDTH,

		//Signal
		DL_COMPOSITE,
		DL_MIX,

		DL_ABOUT_FIRST,
		DL_COUNT = DL_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	static constexpr int kAudioBins = 64;

	/// What the controls resolve to this frame, in physical units. The
	/// harness reads it rather than trusting the slider it set.
	struct Resolved
	{
		double deviation   = 0.0;//MHz per volt
		double dispersal   = 0.0;//MHz p-p
		bool pre           = true;
		bool de            = true;
		double cnrDb       = 0.0;//effective, after the fades
		double audioLevel  = 0.0;
		double ifMHz       = 0.0;
		double videoMHz    = 0.0;
		int seed           = 0;
		int demodulator    = 0;
		int clamp          = 0;
		int composite      = 0;
		double mix         = 1.0;

		int oversample     = downlink::link::kOversample;
		int sub            = downlink::link::kSubPerLink;
		double fineRate    = downlink::link::kFsFine;
		double noiseSigma  = 0.0;//rms of the noise reaching the discriminator, carrier = 1
		double noiseSigmaHz = 0.0;//the Gaussian those taps are, Hz
		std::vector< double > noiseTaps;
		std::vector< double > preTaps;
		std::vector< double > deTaps;
		std::vector< double > lpfTaps;
		double slowAlpha   = 0.0;
		double rest[ 3 ]   = { 0.0, 0.0, 0.0 };
	};
	const Resolved& ResolvedForTest() const
	{
		return resolved;
	}
	const downlink::link::RowTable& RowTableForTest() const
	{
		return rows;
	}

	//--- test hooks: every one is inert in the plugin --------------------------
	/// The host's clock unit, declared rather than inferred.
	void SetClockScaleForTest( double scale );
	/// The uplink's test generator: 0 the clip, 1 a flat level (volts) on
	/// every sample, porches included, 2 a sine per row.
	void SetTestSignalForTest( int mode, float level, float amp = 0.0f, float f0MHz = 0.0f, float stepMHz = 0.0f );
	/// Multiplies the noise; 0 is a noiseless channel.
	void SetNoiseScaleForTest( float scale );
	/// Negative controls.
	void SetLineariseForTest( bool on );
	void SetFlipRotationForTest( bool on );
	void SetSkipClampTauForTest( bool on );
	void SetDeDetuneForTest( double fraction );
	void SetReceiverDeviationDetuneForTest( double fraction );
	/// Discriminator samples per video sample (16 ships; 64 for convergence).
	void SetOversampleForTest( int oversample );
	/// Time every pass with GL timer queries; read after the next frame.
	void SetProfileForTest( bool on );
	std::vector< std::pair< std::string, double > > ProfileForTest();

	enum class Buffer
	{
		Transmit,
		Link,
		Detected,
		Video,
		Porch,
		Clamp,
		Decoded
	};
	/// Read one of the signal buffers back, as RGBA floats, texel row 0 first
	/// (which is scan row 0). Needs the context current.
	bool ReadBufferForTest( Buffer which, std::vector< float >& out, int& width, int& height );
	/// Run the link probe over `count` rows from `firstRow` of the frame last
	/// rendered: ( d, dphi, psi ) per fine sample, texel ( o, r Sub + s ).
	bool ProbeForTest( int firstRow, int count, int channel, std::vector< float >& out );

private:
	double nowSeconds();
	void resolve();
	void setLinkUniforms( ffglex::FFGLShader& shader );

	ffglex::FFGLShader resampleShader, encodeShader, transmitShader, linkShader, probeShader, detectShader, deemphShader,
	    porchShader, clampShader, decodeShader, outputShader;
	ffglex::FFGLScreenQuad quad;

	downlink::PassBuffer resampled, encoded, transmitted, linked, detected, video, porch, clamped, decoded, probe;
	GLuint rowTexture = 0;
	std::vector< float > rowData;
	downlink::link::RowTable rows;
	Resolved resolved;

	//--- the clock (readout's unit voting) --------------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	int clockFrames     = 0;

	//--- test hooks -------------------------------------------------------------
	int testMode         = 0;
	float testLevel      = 0.0f, testAmp = 0.0f, testF0 = 0.0f, testStep = 0.0f;
	float noiseScale     = 1.0f;
	bool linearise       = false;
	bool flipRotation    = false;
	bool skipClampTau    = false;
	double deDetune      = 0.0;
	double rxDevDetune   = 0.0;
	int oversample       = downlink::link::kOversample;
	bool profile         = false;
	std::vector< std::pair< std::string, GLuint > > queries;

	float params[ DL_COUNT ] = {};
	float audioBins[ kAudioBins ] = {};
	std::string aboutText;
};
