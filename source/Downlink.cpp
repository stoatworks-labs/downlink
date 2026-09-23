#include "Downlink.h"

#include "Controls.h"
#include "Diag.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace downlink;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Downlink >,
	"DL01",
	"SW Downlink",
	2,
	1,
	0,
	1,
	FF_EFFECT,
	"Analogue FM satellite television.\n\nThe clip is encoded to PAL, pre-emphasised, and frequency-modulated onto a carrier swept by the 25 Hz energy dispersal triangle. The carrier crosses a noisy link into a Gaussian IF and an FM discriminator, then de-emphasis, a video lowpass, a clamp and the PAL decoder.\n\nWhat falls out: fine coloured grain above threshold; below it the discriminator's clicks, the white and black sparklies, at Rice's rate, more black ones on white; and with a bad clamp the picture rocking at the field rate.",
	"Downlink FFGL effect" );

namespace
{
constexpr int kClockVotes = 4;

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// The compiler's own words for a fragment shader that will not compile. The
/// SDK logs them to the host only; the log file is where they are needed.
std::string fragmentLog( const char* source )
{
	GLuint shader = glCreateShader( GL_FRAGMENT_SHADER );
	glShaderSource( shader, 1, &source, nullptr );
	glCompileShader( shader );
	GLint length = 0;
	glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &length );
	std::string log( static_cast< size_t >( std::max( length, 1 ) ), '\0' );
	if( length > 0 )
		glGetShaderInfoLog( shader, length, nullptr, log.data() );
	glDeleteShader( shader );
	return log;
}

void uniformArray( FFGLShader& shader, const char* name, const std::vector< double >& values, int capacity )
{
	float buffer[ 256 ] = {};
	const int n         = std::min( { static_cast< int >( values.size() ), capacity, 256 } );
	for( int i = 0; i < n; ++i )
		buffer[ i ] = static_cast< float >( values[ static_cast< size_t >( i ) ] );
	glUniform1fv( shader.FindUniform( name ), n, buffer );
}

void uniformInt( FFGLShader& shader, const char* name, int value )
{
	glUniform1i( shader.FindUniform( name ), value );
}

void uniformFloat( FFGLShader& shader, const char* name, double value )
{
	glUniform1f( shader.FindUniform( name ), static_cast< float >( value ) );
}

void uniformVec3( FFGLShader& shader, const char* name, const double* v )
{
	glUniform3f( shader.FindUniform( name ), static_cast< float >( v[ 0 ] ), static_cast< float >( v[ 1 ] ),
	             static_cast< float >( v[ 2 ] ) );
}

/// Draw one full-buffer pass into `target`.
template< typename Body >
void pass( PassBuffer& target, FFGLShader& shader, FFGLScreenQuad& quad, Body&& body )
{
	ScopedFBOBinding fbo( target.GetGLID(), ScopedFBOBinding::RB_REVERT );
	target.ResizeViewPort();
	ScopedShaderBinding binding( shader.GetGLID() );
	//The body binds its inputs and draws: its scoped bindings must be alive
	//for the draw, and they unbind (to 0, not to what was there) on its exit.
	body();
	(void)quad;
}
} // namespace

//---------------------------------------------------------------------------
Downlink::Downlink()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The dispersal is locked to the field on air, and the noise changes with
	//it: the effect needs the host's clock so a re-render is the same picture.
	SetTimeSupported( true );

	params[ DL_DEVIATION ]       = controls::DeviationParam( 13.5f );
	params[ DL_DISPERSAL ]       = controls::DispersalParam( 2.0f );
	params[ DL_PRE_EMPHASIS ]    = 1.0f;
	params[ DL_CNR ]             = controls::CnrParam( 9.0f );
	params[ DL_RAIN ]            = 0.0f;
	params[ DL_AUDIO_FADE ]      = 0.0f;
	params[ DL_IF_BANDWIDTH ]    = controls::IfBandwidthParam( 27.0f );
	params[ DL_SEED ]            = 1.0f;
	params[ DL_DEMODULATOR ]     = static_cast< float >( controls::kDiscriminator );
	params[ DL_CLAMP ]           = static_cast< float >( controls::kClampGood );
	params[ DL_DE_EMPHASIS ]     = 1.0f;
	params[ DL_VIDEO_BANDWIDTH ] = controls::VideoBandwidthParam( 5.0f );
	params[ DL_COMPOSITE ]       = static_cast< float >( controls::kPal );
	params[ DL_MIX ]             = 1.0f;

	auto declareOptions = [ this ]( unsigned int id, const char* name, const char* ( *nameOf )( int ), int count ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameOf( i ), static_cast< float >( i ) );
	};

	SetParamInfof( DL_DEVIATION, "Deviation", FF_TYPE_STANDARD );
	SetParamInfof( DL_DISPERSAL, "Dispersal", FF_TYPE_STANDARD );
	SetParamInfo( DL_PRE_EMPHASIS, "Pre-emphasis On", FF_TYPE_BOOLEAN, true );

	SetParamInfof( DL_CNR, "CNR", FF_TYPE_STANDARD );
	SetParamInfof( DL_RAIN, "Rain Fade", FF_TYPE_STANDARD );
	SetParamInfof( DL_AUDIO_FADE, "Audio Fade", FF_TYPE_STANDARD );
	//Resolume fills a buffer parameter of usage FFT with 64 bins a frame.
	SetBufferParamInfo( DL_AUDIO, "Audio", kAudioBins, FF_USAGE_FFT );
	for( int i = 0; i < kAudioBins; ++i )
		SetParamElementInfo( DL_AUDIO, static_cast< unsigned int >( i ), "", 0.0f );
	SetParamInfof( DL_IF_BANDWIDTH, "IF Bandwidth", FF_TYPE_STANDARD );
	SetParamInfo( DL_SEED, "Noise Seed", FF_TYPE_INTEGER, params[ DL_SEED ] );
	SetParamRange( DL_SEED, 0.0f, 999.0f );

	declareOptions( DL_DEMODULATOR, "Demodulator", controls::DemodulatorName, controls::kDemodulatorCount );
	declareOptions( DL_CLAMP, "Clamp", controls::ClampName, controls::kClampCount );
	SetParamInfo( DL_DE_EMPHASIS, "De-emphasis On", FF_TYPE_BOOLEAN, true );
	SetParamInfof( DL_VIDEO_BANDWIDTH, "Video Bandwidth", FF_TYPE_STANDARD );

	declareOptions( DL_COMPOSITE, "Composite", controls::CompositeName, controls::kCompositeCount );
	SetParamInfof( DL_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = DL_DEVIATION; i <= DL_PRE_EMPHASIS; ++i )
		SetParamGroup( i, "Uplink" );
	for( FFUInt32 i = DL_CNR; i <= DL_SEED; ++i )
		SetParamGroup( i, "Link" );
	for( FFUInt32 i = DL_DEMODULATOR; i <= DL_VIDEO_BANDWIDTH; ++i )
		SetParamGroup( i, "Receiver" );
	for( FFUInt32 i = DL_COMPOSITE; i <= DL_MIX; ++i )
		SetParamGroup( i, "Signal" );

	SetParamInfo( DL_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = DL_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = DL_ABOUT_FIRST; i < DL_COUNT; ++i )
		SetParamGroup( i, "About" );

	resolve();
	FFGLLog::LogToHost( "Created Downlink effect" );
	diag::init();
}

//---------------------------------------------------------------------------
FFResult Downlink::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer="
	            + glStringOrUnknown( GL_RENDERER ) + " version=" + glStringOrUnknown( GL_VERSION ) );

	struct
	{
		FFGLShader* shader;
		const char* fragment;
		const char* name;
	} const stages[] = {
		{ &resampleShader, shaders::kResampleShader, "resample" },
		{ &transmitShader, shaders::kTransmitShader, "transmit" },
		{ &linkShader, shaders::kLinkShader, "link" },
		{ &probeShader, shaders::kLinkProbeShader, "link probe" },
		{ &detectShader, shaders::kDetectShader, "detect" },
		{ &deemphShader, shaders::kDeemphShader, "deemph" },
		{ &porchShader, shaders::kPorchShader, "porch" },
		{ &clampShader, shaders::kClampShader, "clamp" },
		{ &decodeShader, shaders::kDecodeShader, "decode" },
		{ &outputShader, shaders::kOutputShader, "output" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( shaders::kVertexShader, stage.fragment ) )
			continue;
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing: "
		             + fragmentLog( stage.fragment ) );
		FFGLLog::LogToHost( "Downlink: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &rowTexture );
	glBindTexture( GL_TEXTURE_2D, rowTexture );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RG32F, link::kHistoryRows + link::kRows, 1, 0, GL_RG, GL_FLOAT, nullptr );
	glBindTexture( GL_TEXTURE_2D, 0 );

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Downlink::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//readout's unit voting: the ratio of the host's clock delta to a steady
//clock's names the unit outright, and nothing plausible sits between 1 and
//1000.
double Downlink::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;
	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;
			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
// The controls, in physical units, and every filter they imply.
//---------------------------------------------------------------------------
void Downlink::resolve()
{
	Resolved& r = resolved;

	//The level: RMS of the bins, total only. Nobody has measured what the
	//bins are, so nothing here depends on their spacing or their law.
	double sumSquares = 0.0;
	for( float b : audioBins )
		sumSquares += static_cast< double >( b ) * b;
	r.audioLevel = std::min( 1.0, std::sqrt( sumSquares / kAudioBins ) );

	r.deviation   = controls::DeviationMHzPerVolt( params[ DL_DEVIATION ] );
	r.dispersal   = controls::DispersalMHz( params[ DL_DISPERSAL ] );
	r.pre         = params[ DL_PRE_EMPHASIS ] > 0.5f;
	r.de          = params[ DL_DE_EMPHASIS ] > 0.5f;
	r.cnrDb       = controls::CnrDb( params[ DL_CNR ] ) - controls::RainFadeDb( params[ DL_RAIN ] )
	          - controls::AudioFadeDb( params[ DL_AUDIO_FADE ], static_cast< float >( r.audioLevel ) );
	r.ifMHz       = controls::IfBandwidthMHz( params[ DL_IF_BANDWIDTH ] );
	r.videoMHz    = controls::VideoBandwidthMHz( params[ DL_VIDEO_BANDWIDTH ] );
	r.seed        = std::clamp( static_cast< int >( std::lround( params[ DL_SEED ] ) ), 0, 999 );
	r.demodulator = controls::OptionIndex( params[ DL_DEMODULATOR ], controls::kDemodulatorCount );
	r.clamp       = controls::OptionIndex( params[ DL_CLAMP ], controls::kClampCount );
	r.composite   = controls::OptionIndex( params[ DL_COMPOSITE ], controls::kCompositeCount );
	r.mix         = controls::clamp01( params[ DL_MIX ] );

	r.oversample = oversample;
	r.sub        = oversample / link::kLinkFactor;
	r.fineRate   = oversample * link::kFsVideo;

	//The noise reaching the discriminator. The carrier is 1, so the noise rms
	//is 10^( -CNR / 20 ). Threshold extension narrows the noise bandwidth by
	//its stated amount, and with it the noise power.
	const double teFactor = r.demodulator == controls::kThresholdExtension
	                            ? std::pow( 10.0, -link::kThresholdExtensionDb / 10.0 )
	                            : 1.0;
	r.noiseSigmaHz = link::IfSigmaHz( r.ifMHz * 1e6 ) * teFactor;
	r.noiseSigma   = std::pow( 10.0, -r.cnrDb / 20.0 ) * std::sqrt( teFactor ) * noiseScale;
	r.noiseTaps    = link::NoiseTaps( r.noiseSigmaHz, r.fineRate );

	r.preTaps = r.pre ? link::PreEmphasisTaps() : std::vector< double >{ 1.0 };
	if( r.de )
	{
		link::FirstOrder n = link::DeEmphasisNetwork();
		n.pole *= 1.0 + deDetune;
		std::vector< double > taps;
		taps.push_back( n.gain );
		double tail = n.gain * ( n.pole - n.zero );
		const std::vector< double > shipped = link::DeEmphasisTaps();
		for( size_t i = 1; i < shipped.size(); ++i )
		{
			taps.push_back( tail );
			tail *= n.pole;
		}
		r.deTaps = deDetune == 0.0 ? shipped : taps;
	}
	else
		r.deTaps = { 1.0 };
	r.lpfTaps = link::VideoLowpassTaps( r.videoMHz * 1e6 );

	r.slowAlpha = skipClampTau ? 1.0 : 1.0 - std::exp( -link::kRowSeconds / link::kSlowClampSeconds );

	r.rest[ 0 ] = link::kRestVolts;
	r.rest[ 1 ] = r.composite == controls::kComponent ? 0.0 : link::kRestVolts;
	r.rest[ 2 ] = r.rest[ 1 ];
}

void Downlink::setLinkUniforms( FFGLShader& shader )
{
	const Resolved& r = resolved;
	uniformInt( shader, "Transmit", 0 );
	uniformInt( shader, "RowTable", 1 );
	uniformInt( shader, "HistoryRows", link::kHistoryRows );
	uniformInt( shader, "LinkWidth", link::kLinkLine );
	//The receiver divides by the deviation it believes in; the test hook makes
	//it believe in the wrong one.
	uniformFloat( shader, "Deviation", r.deviation );
	uniformFloat( shader, "RadPerMHz", 2.0 * 3.14159265358979323846 * 1e6 / r.fineRate );
	uniformInt( shader, "Sub", r.sub );
	uniformInt( shader, "Oversample", r.oversample );
	uniformArray( shader, "NoiseTaps", r.noiseTaps, 97 );
	uniformInt( shader, "NoiseHalf", static_cast< int >( r.noiseTaps.size() / 2 ) );
	uniformFloat( shader, "NoiseSigma", r.noiseSigma );
	uniformInt( shader, "Rotate", r.demodulator == controls::kThresholdExtension ? 0 : 1 );
	glUniform1ui( shader.FindUniform( "Seed" ), static_cast< GLuint >( r.seed ) );
	glUniform1ui( shader.FindUniform( "FieldLow" ), static_cast< GLuint >( static_cast< uint64_t >( rows.field ) & 0xffffffffu ) );
	uniformInt( shader, "Linearise", linearise ? 1 : 0 );
	uniformInt( shader, "FlipRotation", flipRotation ? 1 : 0 );
}

//---------------------------------------------------------------------------
FFResult Downlink::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;
	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//The spectrum, then the controls.
	if( const ParamInfo* info = FindParamInfo( DL_AUDIO ) )
	{
		const size_t bins = std::min< size_t >( info->elements.size(), kAudioBins );
		for( size_t i = 0; i < bins; ++i )
			audioBins[ i ] = std::max( 0.0f, info->elements[ i ].value );
	}
	resolve();
	const Resolved& r = resolved;
	const bool component = r.composite == controls::kComponent;

	const double now = nowSeconds();
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//The field on air and every row's dispersal, in double; frame-relative
	//floats cross into GLSL.
	link::MakeRowTable( now, r.dispersal, rows );
	rowData.resize( rows.base.size() * 2 );
	for( size_t i = 0; i < rows.base.size(); ++i )
	{
		rowData[ 2 * i ]     = rows.base[ i ];
		rowData[ 2 * i + 1 ] = rows.slope[ i ];
	}

	//Every allocation before anything binds a texture: allocating leaves the
	//active unit bound to nothing.
	using S = PassBuffer::Sampling;
	const bool allocated = resampled.Ensure( link::kLinkActive, link::kRows, GL_RGBA32F, S::Nearest )
	                       && transmitted.Ensure( link::kLinkLine, link::kRows, GL_RGBA32F, S::Nearest )
	                       && linked.Ensure( link::kLinkLine, link::kRows, GL_RGBA32F, S::Nearest )
	                       && detected.Ensure( link::kLine, link::kRows, GL_RGBA32F, S::Nearest )
	                       && video.Ensure( link::kLine, link::kRows, GL_RGBA32F, S::Nearest )
	                       && porch.Ensure( 1, link::kRows, GL_RGBA32F, S::Nearest )
	                       && clamped.Ensure( 1, link::kRows, GL_RGBA32F, S::Nearest )
	                       && decoded.Ensure( link::kActive, link::kRows, GL_RGBA32F, S::Linear );
	if( !allocated )
	{
		diag::error( "could not allocate the signal buffers (about 70 MB of float textures)" );
		return FF_FAIL;
	}

	glBindTexture( GL_TEXTURE_2D, rowTexture );
	glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, static_cast< GLsizei >( rows.base.size() ), 1, GL_RG, GL_FLOAT,
	                 rowData.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );
	const double fieldCycles      = link::SubcarrierPhaseForField( rows.field ) / ( 2.0 * 3.14159265358979323846 );
	const double rowCycles        = link::SubcarrierStepPerRow() / ( 2.0 * 3.14159265358979323846 );

	//1. resample
	pass( resampled, resampleShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( input.Handle );
		uniformInt( resampleShader, "InputTexture", 0 );
		resampleShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		resampleShader.Set( "InputSize", static_cast< float >( input.Width ), static_cast< float >( input.Height ) );
		resampleShader.Set( "TargetSize", static_cast< float >( link::kLinkActive ), static_cast< float >( link::kRows ) );
		quad.Draw();
	} );

	//2. transmit
	pass( transmitted, transmitShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( resampled.TextureID() );
		uniformInt( transmitShader, "Picture", 0 );
		uniformArray( transmitShader, "Pre", r.preTaps, link::kMaxPreTaps );
		uniformInt( transmitShader, "PreCount", static_cast< int >( std::min< size_t >( r.preTaps.size(), link::kMaxPreTaps ) ) );
		uniformInt( transmitShader, "ComponentMode", component ? 1 : 0 );
		uniformVec3( transmitShader, "Rest", r.rest );
		uniformFloat( transmitShader, "FieldCycles", fieldCycles );
		uniformFloat( transmitShader, "RowCycles", rowCycles );
		uniformFloat( transmitShader, "LinkCycles", link::kFsc / link::kFsLink );
		uniformInt( transmitShader, "TestMode", testMode );
		uniformFloat( transmitShader, "TestLevel", testLevel );
		uniformFloat( transmitShader, "TestAmp", testAmp );
		uniformFloat( transmitShader, "TestFreq0", testF0 * 1e6 / link::kFsLink );
		uniformFloat( transmitShader, "TestFreqStep", testStep * 1e6 / link::kFsLink );
		quad.Draw();
	} );

	//3. the link
	pass( linked, linkShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( transmitted.TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( rowTexture );
		setLinkUniforms( linkShader );
		//The receiver's scale: 1 V of video is `deviation` MHz. The negative
		//control detunes what the receiver believes.
		uniformFloat( linkShader, "Deviation", r.deviation );
		uniformInt( linkShader, "Channels", component ? 3 : 1 );
		quad.Draw();
	} );

	//4. detect
	pass( detected, detectShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( linked.TextureID() );
		uniformInt( detectShader, "Link", 0 );
		uniformArray( detectShader, "Lpf", r.lpfTaps, 65 );
		uniformInt( detectShader, "LinkWidth", link::kLinkLine );
		quad.Draw();
	} );

	//5. de-emphasis
	pass( video, deemphShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( detected.TextureID() );
		uniformInt( deemphShader, "Detected", 0 );
		//The receiver's gain error, if the hook asks for one, is applied as
		//a scale on the de-emphasis: the receiver's idea of the deviation.
		std::vector< double > de = r.deTaps;
		for( double& t : de )
			t /= 1.0 + rxDevDetune;
		uniformArray( deemphShader, "De", de, link::kMaxDeTaps );
		uniformInt( deemphShader, "DeCount", static_cast< int >( std::min< size_t >( de.size(), link::kMaxDeTaps ) ) );
		uniformVec3( deemphShader, "Rest", r.rest );
		quad.Draw();
	} );

	//6. porch
	pass( porch, porchShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( video.TextureID() );
		uniformInt( porchShader, "Video", 0 );
		uniformInt( porchShader, "ClampFirst", link::kClampFirst );
		uniformInt( porchShader, "ClampCount", link::kClampCount );
		quad.Draw();
	} );

	//7. clamp
	pass( clamped, clampShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( porch.TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( rowTexture );
		uniformInt( clampShader, "Porch", 0 );
		uniformInt( clampShader, "RowTable", 1 );
		uniformInt( clampShader, "Mode", r.clamp );
		uniformInt( clampShader, "HistoryRows", link::kHistoryRows );
		uniformFloat( clampShader, "Alpha", r.slowAlpha );
		//A blanking porch through the chain: 0 V minus rest, through the
		//emphasis networks' DC gains, plus rest.
		const double preDc = r.pre ? link::PreDcGain() : 1.0;
		const double deDc  = r.de ? 1.0 / link::PreDcGain() : 1.0;
		double porchRest[ 3 ];
		for( int c = 0; c < 3; ++c )
			porchRest[ c ] = r.rest[ c ] - deDc * preDc * r.rest[ c ];
		uniformVec3( clampShader, "PorchRest", porchRest );
		uniformFloat( clampShader, "DispToVolts", deDc / r.deviation );
		uniformFloat( clampShader, "PorchCentre", link::kClampFirst + 0.5 * ( link::kClampCount - 1 ) );
		quad.Draw();
	} );

	//8. decode
	pass( decoded, decodeShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( video.TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( clamped.TextureID() );
		uniformInt( decodeShader, "Video", 0 );
		uniformInt( decodeShader, "Clamp", 1 );
		uniformInt( decodeShader, "ComponentMode", component ? 1 : 0 );
		uniformFloat( decodeShader, "FieldCycles", fieldCycles );
		uniformFloat( decodeShader, "RowCycles", rowCycles );
		uniformFloat( decodeShader, "VideoCycles", link::kFsc / link::kFsVideo );
		uniformFloat( decodeShader, "LumaCutoff", 5.5e6 / link::kFsVideo );
		uniformFloat( decodeShader, "ChromaCutoff", 1.3e6 / link::kFsVideo );
		quad.Draw();
	} );

	//9. output, to the host, in the host's viewport: ScopedFBOBinding puts the
	//framebuffer back and nothing else.
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( outputShader.GetGLID() );
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( input.Handle );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( decoded.TextureID() );
		uniformInt( outputShader, "InputTexture", 0 );
		uniformInt( outputShader, "Decoded", 1 );
		outputShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		uniformFloat( outputShader, "Mix", r.mix );
		quad.Draw();
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Downlink::DeInitGL()
{
	for( FFGLShader* s : { &resampleShader, &transmitShader, &linkShader, &probeShader, &detectShader, &deemphShader,
	                       &porchShader, &clampShader, &decodeShader, &outputShader } )
		s->FreeGLResources();
	quad.Release();
	for( PassBuffer* b : { &resampled, &transmitted, &linked, &detected, &video, &porch, &clamped, &decoded, &probe } )
		b->Destroy();
	if( rowTexture != 0 )
	{
		glDeleteTextures( 1, &rowTexture );
		rowTexture = 0;
	}
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Downlink::SetFloatParameter( unsigned int index, float value )
{
	if( index >= DL_COUNT )
		return FF_FAIL;
	if( index >= DL_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - DL_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;
	params[ index ] = value;
	return FF_SUCCESS;
}

float Downlink::GetFloatParameter( unsigned int index )
{
	return index < DL_COUNT ? params[ index ] : 0.0f;
}

char* Downlink::GetTextParameter( unsigned int index )
{
	if( index == DL_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Downlink::SetTextParameter( unsigned int index, const char* value )
{
	if( index == DL_ABOUT_FIRST )
		return FF_SUCCESS;
	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
// Test hooks.
//---------------------------------------------------------------------------
void Downlink::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Downlink::SetTestSignalForTest( int mode, float level, float amp, float f0MHz, float stepMHz )
{
	testMode  = mode;
	testLevel = level;
	testAmp   = amp;
	testF0    = f0MHz;
	testStep  = stepMHz;
}

void Downlink::SetNoiseScaleForTest( float scale )
{
	noiseScale = scale;
}
void Downlink::SetLineariseForTest( bool on )
{
	linearise = on;
}
void Downlink::SetFlipRotationForTest( bool on )
{
	flipRotation = on;
}
void Downlink::SetSkipClampTauForTest( bool on )
{
	skipClampTau = on;
}
void Downlink::SetDeDetuneForTest( double fraction )
{
	deDetune = fraction;
}
void Downlink::SetReceiverDeviationDetuneForTest( double fraction )
{
	rxDevDetune = fraction;
}
void Downlink::SetOversampleForTest( int value )
{
	oversample = std::clamp( value, 2, 64 ) / 2 * 2;
}

bool Downlink::ReadBufferForTest( Buffer which, std::vector< float >& out, int& width, int& height )
{
	PassBuffer* b = nullptr;
	switch( which )
	{
	case Buffer::Transmit: b = &transmitted; break;
	case Buffer::Link: b = &linked; break;
	case Buffer::Detected: b = &detected; break;
	case Buffer::Video: b = &video; break;
	case Buffer::Porch: b = &porch; break;
	case Buffer::Clamp: b = &clamped; break;
	case Buffer::Decoded: b = &decoded; break;
	}
	if( b == nullptr || !b->IsValid() )
		return false;
	const FFGLTextureStruct info = b->GetTextureInfo();
	width                        = static_cast< int >( info.Width );
	height                       = static_cast< int >( info.Height );
	out.assign( static_cast< size_t >( width ) * height * 4, 0.0f );
	glBindTexture( GL_TEXTURE_2D, b->TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, out.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return true;
}

bool Downlink::ProbeForTest( int firstRow, int count, int channel, std::vector< float >& out )
{
	const Resolved& r = resolved;
	const int h       = count * r.sub;
	if( !probe.Ensure( link::kLinkLine, h, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
		return false;

	GLint previous[ 4 ];
	glGetIntegerv( GL_VIEWPORT, previous );
	pass( probe, probeShader, quad, [ & ] {
		ScopedSamplerActivation s0( 0 );
		Scoped2DTextureBinding t0( transmitted.TextureID() );
		ScopedSamplerActivation s1( 1 );
		Scoped2DTextureBinding t1( rowTexture );
		setLinkUniforms( probeShader );
		uniformInt( probeShader, "ProbeRow0", firstRow );
		uniformInt( probeShader, "ProbeChannel", channel );
		quad.Draw();
	} );
	glViewport( previous[ 0 ], previous[ 1 ], previous[ 2 ], previous[ 3 ] );

	out.assign( static_cast< size_t >( link::kLinkLine ) * h * 4, 0.0f );
	glBindTexture( GL_TEXTURE_2D, probe.TextureID() );
	glPixelStorei( GL_PACK_ALIGNMENT, 1 );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, out.data() );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return true;
}
