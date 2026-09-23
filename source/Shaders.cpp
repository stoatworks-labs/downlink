#include "Shaders.h"

namespace downlink::shaders
{

const char* const kVertexShader = R"(#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
)";

//---------------------------------------------------------------------------
// 1. resample. old-cathode's box filter, with the rows flipped so scan row 0
// is the top of the clip.
//---------------------------------------------------------------------------
const char* const kResampleShader = R"(#version 410 core
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform vec2 InputSize;
uniform vec2 TargetSize;

in vec2 uv;
out vec4 fragColor;

void main()
{
	//This texel's centre, in picture space with y DOWN the picture.
	vec2 at    = vec2( gl_FragCoord.x / TargetSize.x, 1.0 - gl_FragCoord.y / TargetSize.y );
	vec2 ratio = InputSize / max( TargetSize, vec2( 1.0 ) );

	//One tap per source texel covered, capped: the cap only bites past an
	//8:1 reduction, and this pass runs at the link raster.
	ivec2 taps = ivec2( clamp( ceil( ratio ), vec2( 1.0 ), vec2( 6.0 ) ) );
	vec2 texel = 1.0 / max( InputSize, vec2( 1.0 ) );

	vec4 sum = vec4( 0.0 );
	for( int y = 0; y < taps.y; ++y )
	{
		for( int x = 0; x < taps.x; ++x )
		{
			vec2 f = ( vec2( x, y ) + 0.5 ) / vec2( taps ) - 0.5;
			sum += texture( InputTexture, clamp( at + f * ratio * texel, vec2( 0.0 ), vec2( 1.0 ) ) * MaxUV );
		}
	}
	vec4 color = sum / float( taps.x * taps.y );

	//Straight colour: the encoder measures luminance, and a pixel that is
	//dark only because it is transparent is not a dark picture.
	if( color.a > 0.0 )
		color.rgb /= color.a;
	fragColor = color;
}
)";

//---------------------------------------------------------------------------
// 2. transmit. The uplink's baseband and its pre-emphasis.
//---------------------------------------------------------------------------
const char* const kEncodeShader = R"(#version 410 core
uniform sampler2D Picture;//1842 x 576, straight RGBA, scan row = texel row

uniform int ComponentMode;
uniform vec3 Rest;//the carrier's rest level per channel, volts

//The subcarrier, in CYCLES so every product stays small: the field's phase
//is reduced in double on the CPU.
uniform float FieldCycles;
uniform float RowCycles; //0.7516 of a cycle a line
uniform float LinkCycles;//1/8: the link rate is 8 fsc

//The uplink's test generator (harness only; TestMode 0 in the plugin).
uniform int TestMode;     //1 flat, 2 one sine per row
uniform float TestLevel;  //volts
uniform float TestAmp;    //volts
uniform float TestFreq0;  //cycles per link sample, row 0
uniform float TestFreqStep;

in vec2 uv;
out vec4 fragColor;

const int PORCH  = 128;
const int ACTIVE = 1842;

vec3 rgbToYuv( vec3 c )
{
	return vec3(
		dot( c, vec3( 0.299, 0.587, 0.114 ) ),
		dot( c, vec3( -0.14713, -0.28886, 0.436 ) ),
		dot( c, vec3( 0.615, -0.51499, -0.10001 ) ) );
}

vec3 baseband( int n, int y )
{
	if( TestMode == 1 )
		return vec3( TestLevel );
	if( TestMode == 2 )
	{
		float cycles = fract( ( TestFreq0 + TestFreqStep * float( y ) ) * float( n ) );
		return vec3( TestLevel + TestAmp * sin( 6.28318530718 * cycles ) );
	}

	//The porches are at blanking.
	if( n < PORCH || n >= PORCH + ACTIVE )
		return vec3( 0.0 );

	vec3 yuv = rgbToYuv( texelFetch( Picture, ivec2( n - PORCH, y ), 0 ).rgb );
	if( ComponentMode == 1 )
		return 0.7 * yuv;

	//old-cathode's PAL encoder: U on sine, V on cosine, V inverted on
	//alternate lines. 0.7 V from blanking to white.
	float phase = 6.28318530718 * fract( FieldCycles + RowCycles * float( y ) + LinkCycles * float( n - PORCH ) );
	float vSign = ( y & 1 ) == 1 ? -1.0 : 1.0;
	return vec3( 0.7 * ( yuv.x + yuv.y * sin( phase ) + yuv.z * vSign * cos( phase ) ) );
}

//The baseband in volts, less the carrier's rest level: what the modulator
//deviates from.
void main()
{
	fragColor = vec4( baseband( int( gl_FragCoord.x ), int( gl_FragCoord.y ) ) - Rest, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 3. pre-emphasis, causal, at the link rate. Before the line starts the
// signal is what the first sample is (the porch, or the test level).
//---------------------------------------------------------------------------
const char* const kPreemphShader = R"(#version 410 core
uniform sampler2D Encoded;//2002 x 576, volts less rest
uniform float Pre[ 64 ];
uniform int PreCount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	int n    = int( gl_FragCoord.x );
	int y    = int( gl_FragCoord.y );
	vec3 acc = vec3( 0.0 );
	for( int i = 0; i < PreCount; ++i )
		acc += Pre[ i ] * texelFetch( Encoded, ivec2( max( n - i, 0 ), y ), 0 ).rgb;
	fragColor = vec4( acc, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 3. the link. The library is shared, text for text, by the plugin's pass and
// the harness's probe.
//---------------------------------------------------------------------------
#define DOWNLINK_LINK_LIBRARY R"(#version 410 core
//The FM link, per link sample, in parallel.
//
//The carrier is exp( j phi ), phi advancing by dphi[ l ] = 2 pi f[ l ] / fs
//between fine samples, where f is Deviation x the pre-emphasised video plus
//the dispersal. The channel adds complex Gaussian noise n[ l ], shaped by the
//IF. The discriminator's output is
//
//    d[ k ] = arg( r[ k ] conj( r[ k-1 ] ) ),     r = exp( j phi ) + n.
//
//Write the noise in the carrier's own frame, m[ k ] = n[ k ] exp( -j phi[ k ] ).
//Then r[ k ] = exp( j phi[ k ] ) ( 1 + m[ k ] ) and
//
//    d[ k ] = wrap( dphi[ k ] + psi[ k ] - psi[ k-1 ] ),   psi = arg( 1 + m ).
//
//exactly -- no approximation. psi is the phase error; when it passes +/-pi
//the wrap throws a whole cycle, and that is a click.
//
//m needs only phase DIFFERENCES. IF noise is a filter over white noise,
//n[ k ] = sum h[ i ] u'[ k-i ], and white circular noise is rotation
//invariant, so u'[ l ] = u[ l ] exp( j theta[ l ] ) is as white as u for any
//fixed theta. Take theta as the carrier's phase measured from a local origin a
//few samples back:
//
//    m[ k ] = exp( -j theta[ k ] ) sum h[ i ] u[ k-i ] exp( j theta[ k-i ] )
//
//and every theta enters as a difference, so the origin cancels: two pixels
//that both need psi[ k ] get the same number, whatever origin each chose. The
//window is the filter's half-length either side and never more, so nothing is
//ever integrated along a line -- no float ever holds a line's worth of phase.
//
//Threshold Ext. is a tracking filter: a filter that follows the carrier
//perfectly, so it sits in the carrier's frame and needs no rotation at all;
//the CPU hands it the narrower taps and the smaller noise.

uniform sampler2D Transmit;//2002 x 576: pre-emphasised video less rest, volts
uniform sampler2D RowTable;//( history + 576 ) x 1: dispersal MHz, MHz per video sample
uniform int HistoryRows;
uniform int LinkWidth;
uniform float Deviation;   //MHz per volt
uniform float RadPerMHz;   //2 pi 1e6 / fine rate
uniform int Sub;           //fine samples per link sample
uniform int Oversample;    //fine samples per video sample
uniform float NoiseTaps[ 97 ];
uniform int NoiseHalf;
uniform float NoiseSigma;  //noise rms over carrier amplitude
uniform int Rotate;        //1 IF noise, rotated into the carrier's frame; 0 tracking filter
uniform uint Seed;
uniform uint FieldLow;
//Test hooks: always 0 in the plugin. They exist so the harness can prove its
//checks fail on a broken model.
uniform int Linearise;     //psi = Im( m ), unwrapped: a discriminator with no clicks
uniform int FlipRotation;  //rotate the noise the wrong way round

const int MAXW = 128;//P Sub + 1 + 2 NoiseHalf; the CPU keeps every use inside it

uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//Circular complex Gaussian, E|u|^2 = 1 (|u|^2 is exponential), turned
//through `turn` radians: one sincos for the draw and the rotation together.
vec2 gaussian( uint key, int l, float turn )
{
	uint h1  = pcg( key + uint( l + 65536 ) );
	uint h2  = pcg( h1 ^ 0x68bc21ebu );
	float u1 = ( float( h1 >> 8u ) + 0.5 ) / 16777216.0;
	float u2 = float( h2 >> 8u ) / 16777216.0;
	float r  = sqrt( -log( u1 ) );
	float a  = 6.28318530718 * u2 + turn;
	return r * vec2( cos( a ), sin( a ) );
}

//The instantaneous frequency at fine sample l, MHz: Catmull-Rom between link
//samples (flat to 0.05 dB at 5 MHz), plus the dispersal along the row. The
//four link samples are fetched once per link sample, not once per fine one:
//`cached` holds them for link sample `at`.
float frequency( int l, int y, int c, vec2 row, inout int at, inout vec4 cached )
{
	float p  = float( l ) / float( Sub );
	float fi = floor( p );
	float t  = p - fi;
	int i    = int( fi );
	if( i != at )
	{
		at = i;
		cached = vec4( texelFetch( Transmit, ivec2( clamp( i - 1, 0, LinkWidth - 1 ), y ), 0 )[ c ],
		               texelFetch( Transmit, ivec2( clamp( i, 0, LinkWidth - 1 ), y ), 0 )[ c ],
		               texelFetch( Transmit, ivec2( clamp( i + 1, 0, LinkWidth - 1 ), y ), 0 )[ c ],
		               texelFetch( Transmit, ivec2( clamp( i + 2, 0, LinkWidth - 1 ), y ), 0 )[ c ] );
	}
	float x0 = cached.x, x1 = cached.y, x2 = cached.z, x3 = cached.w;
	float v  = x1 + 0.5 * t * ( ( x2 - x0 ) + t * ( ( 2.0 * x0 - 5.0 * x1 + 4.0 * x2 - x3 ) + t * ( 3.0 * ( x1 - x2 ) + x3 - x0 ) ) );
	return Deviation * v + row.x + row.y * float( l ) / float( Oversample );
}

float wrapPi( float x )
{
	return x - 6.28318530718 * floor( ( x + 3.14159265359 ) / 6.28318530718 );
}

//The discriminator over link samples o0 .. o0 + P - 1 (P <= 4) of row y,
//channel c. Link sample o is the sum of its Sub fine outputs d[ k ],
//k = o Sub - Sub/2 + 1 + s for s < Sub, in volts of pre-emphasised video:
//the phase change from o Sub - Sub/2 to o Sub + Sub/2, centred on link
//sample o's own instant, fine sample o Sub. Off centre by even half a link
//sample would delay the chroma against the decoder's reference and turn
//every hue. When probeS is one of those s (and P is 1), probe gets
//( d, dphi, psi ) there.
//
//P link samples share one noise window, so a fragment computing four of them
//draws Sub P + 1 + 2 NoiseHalf fine samples of noise rather than four times
//Sub + 1 + 2 NoiseHalf.
vec4 linkRun( int o0, int P, int y, int c, int probeS, out vec3 probe )
{
	probe    = vec3( 0.0 );
	vec2 row = texelFetch( RowTable, ivec2( HistoryRows + y, 0 ), 0 ).xy;
	uint key = pcg( pcg( pcg( Seed * 4u + uint( c ) ) ^ FieldLow ) + uint( y ) * 2654435769u );

	int lo    = o0 * Sub - Sub / 2 - NoiseHalf;
	int n     = P * Sub + 1 + 2 * NoiseHalf;
	float rot = FlipRotation == 1 ? -1.0 : 1.0;

	float th[ MAXW ];
	vec2 w[ MAXW ];
	float theta   = 0.0;
	int at        = -1 << 30;
	vec4 cached   = vec4( 0.0 );
	for( int q = 0; q < n; ++q )
	{
		int l = lo + q;
		if( q > 0 )
			theta += RadPerMHz * frequency( l, y, c, row, at, cached );
		th[ q ] = theta;
		w[ q ]  = gaussian( key, l, Rotate == 1 ? rot * theta : 0.0 );
	}

	vec4 sums     = vec4( 0.0 );
	float prevPsi = 0.0;
	for( int s = 0; s <= P * Sub; ++s )
	{
		int q  = NoiseHalf + s;
		vec2 m = vec2( 0.0 );
		for( int i = -NoiseHalf; i <= NoiseHalf; ++i )
			m += NoiseTaps[ i + NoiseHalf ] * w[ q - i ];
		if( Rotate == 1 )
		{
			float cs = cos( th[ q ] );
			float sn = rot * sin( th[ q ] );
			m        = vec2( m.x * cs + m.y * sn, m.y * cs - m.x * sn );
		}
		m *= NoiseSigma;

		vec2 r    = vec2( 1.0 + m.x, m.y );
		float psi = Linearise == 1 ? m.y : atan( r.y, r.x );
		if( s > 0 )
		{
			float dphi = th[ q ] - th[ q - 1 ];
			float d    = Linearise == 1 ? dphi + psi - prevPsi : wrapPi( dphi + psi - prevPsi );
			int p      = ( s - 1 ) / Sub;
			sums[ p ] += d;
			if( P == 1 && s - 1 == probeS )
				probe = vec3( d, dphi, psi );
		}
		prevPsi = psi;
	}
	return sums / ( RadPerMHz * float( Sub ) * Deviation );
}
)"

const char* const kLinkShader = DOWNLINK_LINK_LIBRARY R"(
uniform int PackedWidth;//link samples / 4, rounded up: one channel's block
uniform int PerRun;     //4, or fewer when a test's sampling rate would not fit MAXW

in vec2 uv;
out vec4 fragColor;

//Four link samples per texel, one channel per block of PackedWidth texels.
void main()
{
	int x  = int( gl_FragCoord.x );
	int y  = int( gl_FragCoord.y );
	int c  = x / PackedWidth;
	int o0 = 4 * ( x - c * PackedWidth );
	vec4 v = vec4( 0.0 );
	vec3 unused;
	for( int b = 0; b < 4; b += PerRun )
	{
		int count = min( PerRun, LinkWidth - ( o0 + b ) );
		if( count <= 0 )
			break;
		vec4 part = linkRun( o0 + b, count, y, c, -1, unused );
		for( int i = 0; i < PerRun; ++i )
			if( b + i < 4 )
				v[ b + i ] = part[ i ];
	}
	fragColor = v;
}
)";

const char* const kLinkProbeShader = DOWNLINK_LINK_LIBRARY R"(
uniform int ProbeRow0;
uniform int ProbeChannel;

in vec2 uv;
out vec4 fragColor;

//One texel per FINE sample: x is the link sample, y is ( row, sub-sample ).
void main()
{
	int o  = int( gl_FragCoord.x );
	int py = int( gl_FragCoord.y );
	vec3 probe;
	linkRun( o, 1, ProbeRow0 + py / Sub, ProbeChannel, py % Sub, probe );
	fragColor = vec4( probe, 0.0 );
}
)";

//---------------------------------------------------------------------------
// 4. detect: the video lowpass at the link rate, then every second sample.
//---------------------------------------------------------------------------
const char* const kDetectShader = R"(#version 410 core
uniform sampler2D Link;//( PackedWidth x channels ) x 576, four link samples a texel
uniform float Lpf[ 65 ];
uniform int LinkWidth;
uniform int PackedWidth;
uniform int Channels;

in vec2 uv;
out vec4 fragColor;

float linkAt( int o, int y, int c )
{
	o = clamp( o, 0, LinkWidth - 1 );
	return texelFetch( Link, ivec2( c * PackedWidth + o / 4, y ), 0 )[ o % 4 ];
}

void main()
{
	int j    = int( gl_FragCoord.x );
	int y    = int( gl_FragCoord.y );
	vec4 acc = vec4( 0.0 );
	for( int c = 0; c < Channels; ++c )
		for( int i = -32; i <= 32; ++i )
			acc[ c ] += Lpf[ i + 32 ] * linkAt( 2 * j - i, y, c );
	fragColor = acc;
}
)";

//---------------------------------------------------------------------------
// 5. de-emphasis, at the video rate, and the rest level put back.
//---------------------------------------------------------------------------
const char* const kDeemphShader = R"(#version 410 core
uniform sampler2D Detected;//1001 x 576
uniform float De[ 128 ];
uniform int DeCount;
uniform vec3 Rest;

in vec2 uv;
out vec4 fragColor;

void main()
{
	int j    = int( gl_FragCoord.x );
	int y    = int( gl_FragCoord.y );
	vec3 acc = vec3( 0.0 );
	for( int n = 0; n < DeCount; ++n )
		acc += De[ n ] * texelFetch( Detected, ivec2( max( j - n, 0 ), y ), 0 ).rgb;
	fragColor = vec4( Rest + acc, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 6. the back porch, as demodulated.
//---------------------------------------------------------------------------
const char* const kPorchShader = R"(#version 410 core
uniform sampler2D Video;//1001 x 576
uniform int ClampFirst;
uniform int ClampCount;

in vec2 uv;
out vec4 fragColor;

void main()
{
	int y    = int( gl_FragCoord.y );
	vec3 acc = vec3( 0.0 );
	for( int x = 0; x < ClampCount; ++x )
		acc += texelFetch( Video, ivec2( ClampFirst + x, y ), 0 ).rgb;
	fragColor = vec4( acc / float( ClampCount ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// 7. the clamp. What comes off each row.
//
// Good is a keyed clamp: this row's porch. Slow is a DC restorer with a time
// constant: c[ y ] = c[ y-1 ] + alpha ( p[ y ] - c[ y-1 ] ), a recursion down
// the field, written out as the sum it is so every row is its own fragment.
// Before row 0 there is the vertical interval and the tail of the previous
// field, whose porches carry the dispersal and no picture; those are taken
// from the row table in closed form (noise-free: AGENTS.md says why).
//---------------------------------------------------------------------------
const char* const kClampShader = R"(#version 410 core
uniform sampler2D Porch;   //1 x 576
uniform sampler2D RowTable;//( history + 576 ) x 1
uniform int Mode;          //0 Off, 1 Slow, 2 Good
uniform int HistoryRows;
uniform float Alpha;       //1 - exp( -T_row / tau )
uniform vec3 PorchRest;    //a blanking porch's level through the chain, volts
uniform float DispToVolts; //MHz of dispersal to volts of video
uniform float PorchCentre; //the clamp window's centre, video samples

in vec2 uv;
out vec4 fragColor;

vec3 porchAt( int j )
{
	if( j >= 0 )
		return texelFetch( Porch, ivec2( 0, j ), 0 ).rgb;
	vec2 row = texelFetch( RowTable, ivec2( HistoryRows + j, 0 ), 0 ).xy;
	return PorchRest + vec3( DispToVolts * ( row.x + row.y * PorchCentre ) );
}

void main()
{
	int y = int( gl_FragCoord.y );
	vec3 c = vec3( 0.0 );
	if( Mode == 2 )
		c = porchAt( y );
	else if( Mode == 1 )
	{
		float keep = 1.0;
		for( int m = 0; m < HistoryRows; ++m )
		{
			c += Alpha * keep * porchAt( y - m );
			keep *= 1.0 - Alpha;
		}
		c += keep * porchAt( y - HistoryRows );
	}
	fragColor = vec4( c, 1.0 );
}
)";

//---------------------------------------------------------------------------
// 8. decode. old-cathode's decodeLine, reading the received composite instead
// of encoding it on the fly.
//---------------------------------------------------------------------------
const char* const kDecodeShader = R"(#version 410 core
uniform sampler2D Video;//1001 x 576, volts
uniform sampler2D Clamp;//1 x 576
uniform int ComponentMode;
uniform float FieldCycles;
uniform float RowCycles;
uniform float VideoCycles;//1/4: the video rate is 4 fsc
uniform float LumaCutoff; //cycles per video sample
uniform float ChromaCutoff;

in vec2 uv;
out vec4 fragColor;

const float PI  = 3.14159265359;
const int TAPS  = 8;
const int PORCH = 64;

vec3 yuvToRgb( vec3 c )
{
	return vec3(
		c.x + 1.13983 * c.z,
		c.x - 0.39465 * c.y - 0.58060 * c.z,
		c.x + 2.03211 * c.y );
}

float windowedSinc( float n, float cutoff )
{
	float x = 2.0 * cutoff * n;
	float s = abs( x ) < 1e-6 ? 1.0 : sin( PI * x ) / ( PI * x );
	float w = max( 0.5 + 0.5 * cos( PI * n / float( TAPS + 1 ) ), 0.0 );
	return 2.0 * cutoff * s * w;
}

float composite( int n, int y )
{
	return texelFetch( Video, ivec2( PORCH + n, y ), 0 ).r - texelFetch( Clamp, ivec2( 0, y ), 0 ).r;
}

//Synchronous demodulation against a perfect local reference, the luma
//notched at the subcarrier (at 4 fsc, samples two apart are half a cycle
//apart, so the notch is a three-tap average).
vec3 decodeLine( int n, int y )
{
	float vSign = ( y & 1 ) == 1 ? -1.0 : 1.0;
	float ySum = 0.0, uSum = 0.0, vSum = 0.0, yW = 0.0, cW = 0.0;
	for( int i = -TAPS; i <= TAPS; ++i )
	{
		float fi    = float( i );
		float comp  = composite( n + i, y );
		float phase = 2.0 * PI * fract( FieldCycles + RowCycles * float( y ) + VideoCycles * float( n + i ) );
		float wy    = 0.25 * windowedSinc( fi - 2.0, LumaCutoff ) + 0.50 * windowedSinc( fi, LumaCutoff )
		           + 0.25 * windowedSinc( fi + 2.0, LumaCutoff );
		float wc    = windowedSinc( fi, ChromaCutoff );
		ySum += comp * wy;
		yW += wy;
		uSum += comp * sin( phase ) * wc;
		vSum += comp * cos( phase ) * wc;
		cW += wc;
	}
	return vec3( ySum / yW, 2.0 * uSum / cW, 2.0 * vSum / cW * vSign ) / 0.7;
}

void main()
{
	int n = int( gl_FragCoord.x );
	int y = int( gl_FragCoord.y );

	vec3 yuv;
	if( ComponentMode == 1 )
		yuv = ( texelFetch( Video, ivec2( PORCH + n, y ), 0 ).rgb - texelFetch( Clamp, ivec2( 0, y ), 0 ).rgb ) / 0.7;
	else
	{
		//The delay line: this line's colour averaged with the line before,
		//whose V was sent inverted, so a phase error costs saturation and not
		//hue.
		yuv        = decodeLine( n, y );
		vec3 above = decodeLine( n, max( y - 1, 0 ) );
		yuv.yz     = 0.5 * ( yuv.yz + above.yz );
	}
	fragColor = vec4( yuvToRgb( yuv ), 1.0 );
}
)";

//---------------------------------------------------------------------------
// 9. output.
//---------------------------------------------------------------------------
const char* const kOutputShader = R"(#version 410 core
uniform sampler2D InputTexture;
uniform sampler2D Decoded;//921 x 576, scan row 0 at texel row 0
uniform vec2 MaxUV;
uniform float Mix;

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec4 clip = texture( InputTexture, uv * MaxUV );
	vec3 pic  = clamp( texture( Decoded, vec2( uv.x, 1.0 - uv.y ) ).rgb, 0.0, 1.0 );
	fragColor = vec4( mix( clip.rgb, pic * clip.a, Mix ), clip.a );
}
)";

const Named kAll[] = {
	{ "vertex", kVertexShader },
	{ "resample", kResampleShader },
	{ "encode", kEncodeShader },
	{ "preemph", kPreemphShader },
	{ "link", kLinkShader },
	{ "linkprobe", kLinkProbeShader },
	{ "detect", kDetectShader },
	{ "deemph", kDeemphShader },
	{ "porch", kPorchShader },
	{ "clamp", kClampShader },
	{ "decode", kDecodeShader },
	{ "output", kOutputShader },
};
const int kAllCount = static_cast< int >( sizeof( kAll ) / sizeof( kAll[ 0 ] ) );

} // namespace downlink::shaders
