/**
 * Downlink — browser demo.
 *
 * Analogue FM satellite television in a page. The one idea, from `AGENTS.md`:
 * FM trades bandwidth for noise, and that trade has a cliff. Build the link —
 * PAL, CCIR 405 pre-emphasis, the FM carrier with its energy dispersal,
 * Gaussian noise in a Gaussian IF, an arg-difference discriminator,
 * de-emphasis, a video lowpass, a clamp, the PAL decoder — and the look falls
 * out of the discriminator's maths: fine coloured grain above threshold, the
 * white and black "sparklies" below it, and with a bad clamp the 25 Hz
 * dispersal triangle rocking the brightness.
 *
 * The plugin is ten GPU passes with a CPU half around them, and the two halves
 * are not equally faithful here:
 *
 *   The shaders are the plugin's. `VERTEX`, `RESAMPLE`, `ENCODE`, `PREEMPH`,
 *   `LINK_LIBRARY`, `LINK_MAIN`, `DETECT`, `DEEMPH`, `PORCH`, `CLAMP`,
 *   `DECODE` and `OUTPUT` below are the literals of `source/Shaders.cpp`,
 *   copied across unedited. The link pass is assembled the way the C++
 *   preprocessor assembles `kLinkShader` — the `DOWNLINK_LINK_LIBRARY` macro's
 *   text followed by the pass's own `main` — so the pieces are carried and
 *   compared separately. `demo/tools/check_shaders.py` compares every piece
 *   character for character and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT — of `Link.cpp` (the F.405 networks, the lowpass,
 *   the Gaussian IF's taps, the dispersal triangle, the per-row table, the
 *   field index and the subcarrier phase), of the conversions in `Controls.h`,
 *   and of what `Downlink.cpp` does every frame (`resolve()`, the clock's unit
 *   voting, and every uniform `ProcessOpenGL` sets), function for function.
 *   It computes in double where the C++ does, and rounds to float
 *   (`Math.fround`) exactly where the C++ types are float. Nothing checks a
 *   port but a reader: `dltest --emphasis`, `--clicks`, `--dispersal`, `--seed`
 *   and the rest check the C++ and have never heard of this page.
 *
 * ------------------------------------------------------- what is missing
 *
 * **There is no audio here.** The plugin declares an `Audio` buffer parameter,
 * 64 FFT bins that Resolume fills every frame, and `Audio Fade` takes up to
 * 12 dB off the link times their RMS level. A browser page has no host to fill
 * the bins, so the level is 0: `Audio Fade` is on the panel, as the plugin
 * declares it, and does nothing. The `Audio` buffer itself is not on the panel
 * — it is not a control a person sets, and the kit has no buffer control.
 *
 * **Noise Seed is a dropdown.** It is `FF_TYPE_INTEGER`, 0 to 999, and the kit
 * has no integer control, so it is a dropdown of all thousand values.
 *
 * **The About block is absent**, as on every page in this suite.
 *
 * **The harness's hooks are absent.** `Set*ForTest` and the link probe
 * (`kLinkProbeShader`) are always off in the plugin and exist so `dltest` can
 * break the model; the page carries the hooks' inert values, not the hooks.
 *
 * ------------------------------------------------------- decided, not asked
 *
 * **The whole link runs, at the plugin's own raster**: 2002 × 576 at the link
 * rate, the discriminator at 283.75 MHz, four link samples a fragment. Nothing
 * is thinned to make the page cheaper, because the thing the plugin is for —
 * clicks at the sampled discriminator's rate — depends on every one of those
 * numbers. On a weak GPU the page is slow rather than wrong.
 *
 * **The clock's unit voting is ported too**, including its first few frames
 * on a steady clock before it settles on seconds. The kit hands the page's
 * clock over in seconds already, so the vote always goes the same way; it is
 * ported because it is what the plugin does with a clock, and a page that
 * skipped it would be a page that disagreed about the first frames.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core — so a pixel here
 * is not evidence about a pixel there.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here.
//
// The two backticks inside the link library's comments are escaped, because a
// template literal has nowhere else to put them; check_shaders.py decodes that
// one escape before comparing and rejects any other backslash, so the escape
// cannot hide a difference.
//---------------------------------------------------------------------------

const VERTEX = `#version 410 core

layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

const RESAMPLE = `#version 410 core
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
`;

const ENCODE = `#version 410 core
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
`;

const PREEMPH = `#version 410 core
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
`;

const LINK_LIBRARY = `#version 410 core
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
//through \`turn\` radians: one sincos for the draw and the rotation together.
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
//\`cached\` holds them for link sample \`at\`.
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
`;

const LINK_MAIN = `
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
`;

const DETECT = `#version 410 core
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
`;

const DEEMPH = `#version 410 core
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
`;

const PORCH = `#version 410 core
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
`;

const CLAMP = `#version 410 core
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
`;

const DECODE = `#version 410 core
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
`;

const OUTPUT = `#version 410 core
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
`;

// `kLinkShader` is `DOWNLINK_LINK_LIBRARY R"( ... )"`: two adjacent string
// literals, which C++ joins into one. This is that join and nothing else.
const LINK = LINK_LIBRARY + LINK_MAIN;

//---------------------------------------------------------------------------
// Float, where the C++ is float.
//
// Controls.h does its arithmetic in `float`, and `params[]` is a float array,
// so every conversion from a slider is rounded to float at each step as the
// C++ types say. Everything in Link.cpp and resolve() is double, as is JS.
// (A compiler that fuses a float multiply-add into one rounding would differ
// from this in the last bit of a float; the plugin's build does not promise
// either way.)
//---------------------------------------------------------------------------
const f32 = Math.fround;

//---------------------------------------------------------------------------
// Controls.h — ported.
//---------------------------------------------------------------------------
const controls = (() => {
  const clamp01 = (v) => f32(Math.min(Math.max(f32(v), 0), 1));
  const linear = (p, lo, hi) => f32(lo + f32(f32(hi - lo) * clamp01(p)));
  const inverseLinear = (v, lo, hi) => clamp01(f32(f32(v - lo) / f32(hi - lo)));

  const kDeviationLo = 4.0;
  const kDeviationHi = 24.0;
  const kDispersalHi = 8.0;
  const kCnrLo = 0.0;
  const kCnrHi = 25.0;
  const kRainHi = 10.0;
  const kAudioFadeHi = 12.0;
  const kIfLo = 18.0;
  const kIfHi = 36.0;
  const kVideoLo = 2.0;
  const kVideoHi = 6.0;

  return {
    clamp01,
    DeviationMHzPerVolt: (p) => linear(p, kDeviationLo, kDeviationHi),
    DeviationParam: (v) => inverseLinear(v, kDeviationLo, kDeviationHi),
    DispersalMHz: (p) => linear(p, 0.0, kDispersalHi),
    DispersalParam: (v) => inverseLinear(v, 0.0, kDispersalHi),
    CnrDb: (p) => linear(p, kCnrLo, kCnrHi),
    CnrParam: (v) => inverseLinear(v, kCnrLo, kCnrHi),
    RainFadeDb: (p) => linear(p, 0.0, kRainHi),
    RainFadeParam: (v) => inverseLinear(v, 0.0, kRainHi),
    // Audio Fade: dB of fade at full level, times the RMS of the host's bins.
    AudioFadeDb: (p, level) => f32(linear(p, 0.0, kAudioFadeHi) * f32(Math.min(Math.max(level, 0), 1))),
    IfBandwidthMHz: (p) => linear(p, kIfLo, kIfHi),
    IfBandwidthParam: (v) => inverseLinear(v, kIfLo, kIfHi),
    VideoBandwidthMHz: (p) => linear(p, kVideoLo, kVideoHi),
    VideoBandwidthParam: (v) => inverseLinear(v, kVideoLo, kVideoHi),

    kDiscriminator: 0,
    kThresholdExtension: 1,
    kDemodulatorCount: 2,
    DemodulatorNames: ['Discriminator', 'Threshold Ext.'],

    kClampOff: 0,
    kClampSlow: 1,
    kClampGood: 2,
    kClampCount: 3,
    ClampNames: ['Off', 'Slow', 'Good'],

    kPal: 0,
    kComponent: 1,
    kCompositeCount: 2,
    CompositeNames: ['PAL', 'Component'],

    // std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 ).
    // lround rounds halves away from zero; Math.round rounds them up, which is
    // the same thing for the non-negative values a host sends.
    OptionIndex: (value, count) => Math.min(Math.max(lround(value), 0), count - 1),
  };
})();

function lround(x) {
  return x < 0 ? -Math.round(-x) : Math.round(x);
}

//---------------------------------------------------------------------------
// Link.h / Link.cpp — ported. Every number and every filter, in double.
//---------------------------------------------------------------------------
const link = (() => {
  const kPi = 3.14159265358979323846;
  const kTau = 2.0 * kPi;

  // the video raster
  const kFsc = 4.43361875e6;
  const kFsVideo = 4.0 * kFsc;
  const kLinkFactor = 2;
  const kFsLink = kLinkFactor * kFsVideo;
  const kOversample = 16;
  const kSubPerLink = kOversample / kLinkFactor;
  const kFsFine = kOversample * kFsVideo;

  const kActive = 921;
  const kBackPorch = 64;
  const kFrontPorch = 16;
  const kLine = kBackPorch + kActive + kFrontPorch;
  const kLinkLine = kLine * kLinkFactor;
  const kLinkActive = kActive * kLinkFactor;
  const kRows = 576;

  const kClampFirst = 16;
  const kClampCount = 32;

  // time
  const kFieldSeconds = 0.02;
  const kLineSeconds = 64e-6;
  const kRowSeconds = kLineSeconds / 2.0;
  const kFirstRowSeconds = 22.0 * kLineSeconds;
  const kDispersalHz = 25.0;

  // levels
  const kRestVolts = 0.2;

  // ITU-R F.405-1, 625 lines
  const kPreA = 11.0;
  const kPreB = 0.4083;
  const kPreC = 10.21;

  const kThresholdExtensionDb = 3.0;
  const kSlowClampSeconds = 2e-3;

  // limits the shaders are compiled for
  const kMaxPreTaps = 64;
  const kMaxDeTaps = 128;
  const kLpfHalf = 32;
  const kMaxNoiseHalf = 47;
  const kLinkWindow = 128;
  const kHistoryRows = 576;

  const tauZeroSeconds = () => (Math.sqrt(kPreC) / kTau) * 1e-6;
  const tauPoleSeconds = () => (Math.sqrt(kPreB) / kTau) * 1e-6;

  function firstOrderTaps(n, cap, floor) {
    const taps = [n.gain];
    let tail = n.gain * (n.pole - n.zero);
    while (taps.length < cap) {
      taps.push(tail);
      tail *= n.pole;
      if (Math.abs(tail) < floor * Math.abs(n.gain)) break;
    }
    return taps;
  }

  const PreDcGain = () => Math.pow(10.0, -kPreA / 20.0);

  function PreEmphasisNetwork(fs = kFsLink) {
    const T = 1.0 / fs;
    const zero = Math.exp(-T / tauZeroSeconds());
    const pole = Math.exp(-T / tauPoleSeconds());
    const gain = (PreDcGain() * (1.0 - pole)) / (1.0 - zero);
    return { gain, zero, pole };
  }

  function DeEmphasisNetwork(fs = kFsVideo) {
    const T = 1.0 / fs;
    const zero = Math.exp(-T / tauPoleSeconds());
    const pole = Math.exp(-T / tauZeroSeconds());
    const gain = ((1.0 / PreDcGain()) * (1.0 - pole)) / (1.0 - zero);
    return { gain, zero, pole };
  }

  const PreEmphasisTaps = (fs = kFsLink) => firstOrderTaps(PreEmphasisNetwork(fs), kMaxPreTaps, 1e-7);
  const DeEmphasisTaps = (fs = kFsVideo) => firstOrderTaps(DeEmphasisNetwork(fs), kMaxDeTaps, 1e-6);

  function VideoLowpassTaps(cutoffHz, fs = kFsLink) {
    const fc = cutoffHz / fs;
    const taps = new Array(2 * kLpfHalf + 1);
    let sum = 0.0;
    for (let i = -kLpfHalf; i <= kLpfHalf; i += 1) {
      const x = 2.0 * fc * i;
      const s = i === 0 ? 1.0 : Math.sin(kPi * x) / (kPi * x);
      const w = 0.5 + 0.5 * Math.cos((kPi * i) / (kLpfHalf + 1.0));
      taps[i + kLpfHalf] = s * w;
      sum += s * w;
    }
    for (let i = 0; i < taps.length; i += 1) taps[i] /= sum;
    return taps;
  }

  const IfSigmaHz = (bandwidthHz) => bandwidthHz / (2.0 * Math.sqrt(2.0 * Math.log(2.0)));

  function NoiseTaps(sigmaHz, fsFine = kFsFine) {
    const sigmaT = fsFine / (kTau * Math.sqrt(2.0) * sigmaHz);
    const half = Math.min(kMaxNoiseHalf, Math.trunc(Math.ceil(4.0 * sigmaT)));
    const taps = new Array(2 * half + 1);
    let energy = 0.0;
    for (let i = -half; i <= half; i += 1) {
      const v = Math.exp(-0.5 * (i / sigmaT) * (i / sigmaT));
      taps[i + half] = v;
      energy += v * v;
    }
    const norm = 1.0 / Math.sqrt(energy);
    for (let i = 0; i < taps.length; i += 1) taps[i] *= norm;
    return taps;
  }

  function Triangle(cycleSeconds, ppMHz) {
    const u = cycleSeconds * kDispersalHz - Math.floor(cycleSeconds * kDispersalHz);
    return u < 0.5 ? ppMHz * (2.0 * u - 0.5) : ppMHz * (1.5 - 2.0 * u);
  }

  // static_cast< int64_t >( std::floor( ... ) ). A double holds any field
  // count a page will reach exactly.
  const FieldIndex = (hostSeconds) => Math.floor(hostSeconds / kFieldSeconds);

  // The field on air and every row's place in the 40 ms triangle cycle. The
  // field index can be huge on a real host, so only its parity crosses into
  // the cycle, and nothing absolute reaches a float.
  function MakeRowTable(hostSeconds, dispersalPpMHz, table) {
    table.field = FieldIndex(hostSeconds);
    table.parity = ((table.field % 2) + 2) % 2;

    const n = kHistoryRows + kRows;
    if (!table.base || table.base.length !== n) {
      table.base = new Float32Array(n);
      table.slope = new Float32Array(n);
      table.cycleSeconds = new Float64Array(n);
    }

    const fieldStart = table.parity * kFieldSeconds;
    const perSample = 1.0 / kFsVideo;
    for (let i = 0; i < n; i += 1) {
      const row = i - kHistoryRows;
      let t = fieldStart + kFirstRowSeconds + row * kRowSeconds;
      const cyc = 2.0 * kFieldSeconds;
      t -= Math.floor(t / cyc) * cyc;
      table.cycleSeconds[i] = t;
      const v0 = Triangle(t, dispersalPpMHz);
      const u = t * kDispersalHz - Math.floor(t * kDispersalHz);
      const ds = (u < 0.5 ? 2.0 : -2.0) * dispersalPpMHz * kDispersalHz * perSample;
      // static_cast< float >: a Float32Array store is that rounding.
      table.base[i] = v0;
      table.slope[i] = ds;
    }
  }

  // PAL: 283.7516 cycles of subcarrier in a 64 us line.
  const SubcarrierStepPerLinkSample = () => (kTau * kFsc) / kFsLink;
  // std::modf's fraction. 283.7516 - 283 is exact in double.
  const SubcarrierStepPerRow = () => kTau * (283.7516 - Math.trunc(283.7516));
  function SubcarrierPhaseForField(field) {
    // JS `%` on doubles is fmod, and on a non-negative int64 it is C++'s `%`.
    const perField = (283.7516 * 312.5) % 1.0;
    const n = field % 1000000;
    return kTau * ((perField * n) % 1.0);
  }

  return {
    kPi, kTau, kFsc, kFsVideo, kLinkFactor, kFsLink, kOversample, kSubPerLink, kFsFine,
    kActive, kLine, kLinkLine, kLinkActive, kRows, kClampFirst, kClampCount,
    kRowSeconds, kRestVolts, kThresholdExtensionDb, kSlowClampSeconds,
    kMaxPreTaps, kMaxDeTaps, kLinkWindow, kHistoryRows,
    PreDcGain, PreEmphasisTaps, DeEmphasisTaps, DeEmphasisNetwork, VideoLowpassTaps,
    IfSigmaHz, NoiseTaps, Triangle, FieldIndex, MakeRowTable,
    SubcarrierStepPerLinkSample, SubcarrierStepPerRow, SubcarrierPhaseForField,
  };
})();

//---------------------------------------------------------------------------
// Downlink::resolve() — ported. The controls in physical units, and every
// filter they imply. `params` here is the page's 0..1 host values, rounded to
// float the way the plugin's `float params[]` holds them.
//
// The test hooks it reads are at the values the plugin always has them at:
// noiseScale 1, deDetune 0 (so the shipped de-emphasis taps), rxDevDetune 0,
// skipClampTau false, oversample 16.
//---------------------------------------------------------------------------
const kAudioBins = 64;

function resolve(p, audioBins) {
  const r = {};

  // The level: RMS of the bins, total only. There are no bins in a browser;
  // the caller passes 64 zeros, so this is 0.
  let sumSquares = 0.0;
  for (const b of audioBins) sumSquares += b * b;
  r.audioLevel = Math.min(1.0, Math.sqrt(sumSquares / kAudioBins));

  r.deviation = controls.DeviationMHzPerVolt(p.deviation);
  r.dispersal = controls.DispersalMHz(p.dispersal);
  r.pre = f32(p.preEmphasis) > 0.5;
  r.de = f32(p.deEmphasis) > 0.5;
  // Three floats subtracted in float, then widened.
  r.cnrDb = f32(f32(controls.CnrDb(p.cnr) - controls.RainFadeDb(p.rain))
    - controls.AudioFadeDb(p.audioFade, f32(r.audioLevel)));
  r.ifMHz = controls.IfBandwidthMHz(p.ifBandwidth);
  r.videoMHz = controls.VideoBandwidthMHz(p.videoBandwidth);
  r.seed = Math.min(Math.max(lround(f32(p.seed)), 0), 999);
  r.demodulator = controls.OptionIndex(f32(p.demodulator), controls.kDemodulatorCount);
  r.clamp = controls.OptionIndex(f32(p.clamp), controls.kClampCount);
  r.composite = controls.OptionIndex(f32(p.composite), controls.kCompositeCount);
  r.mix = controls.clamp01(p.mix);

  const oversample = link.kOversample;
  r.oversample = oversample;
  r.sub = oversample / link.kLinkFactor;
  r.fineRate = oversample * link.kFsVideo;

  // The noise reaching the discriminator. The carrier is 1, so the noise rms
  // is 10^( -CNR / 20 ). Threshold extension narrows the noise bandwidth by
  // its stated amount, and with it the noise power.
  const teFactor = r.demodulator === controls.kThresholdExtension
    ? Math.pow(10.0, -link.kThresholdExtensionDb / 10.0)
    : 1.0;
  const noiseScale = 1.0;
  r.noiseSigmaHz = link.IfSigmaHz(r.ifMHz * 1e6) * teFactor;
  r.noiseSigma = Math.pow(10.0, -r.cnrDb / 20.0) * Math.sqrt(teFactor) * noiseScale;
  r.noiseTaps = link.NoiseTaps(r.noiseSigmaHz, r.fineRate);

  r.preTaps = r.pre ? link.PreEmphasisTaps() : [1.0];
  // deDetune is 0 in the plugin, which takes the shipped taps.
  r.deTaps = r.de ? link.DeEmphasisTaps() : [1.0];
  r.lpfTaps = link.VideoLowpassTaps(r.videoMHz * 1e6);

  r.slowAlpha = 1.0 - Math.exp(-link.kRowSeconds / link.kSlowClampSeconds);

  r.rest = [
    link.kRestVolts,
    r.composite === controls.kComponent ? 0.0 : link.kRestVolts,
    0,
  ];
  r.rest[2] = r.rest[1];
  return r;
}

//---------------------------------------------------------------------------
// Downlink::nowSeconds() — ported: readout's unit voting. The ratio of the
// host's clock delta to a steady clock's names the unit outright. Until four
// frames agree, the plugin runs on its own steady clock.
//
// Here the "host" is the page's clock, which the kit hands over in seconds, so
// the vote goes to seconds within a few frames every time.
//---------------------------------------------------------------------------
const kClockVotes = 4;

class HostClock {
  constructor() {
    this.hostTimeSeen = false;
    this.clockScale = 0.0;
    this.wallStart = -1.0;
    this.lastWallTime = -1.0;
    this.lastRawTime = -1.0;
    this.secondsVotes = 0;
    this.millisVotes = 0;
  }

  // SetTime(): the kit calls render() with a clock every frame.
  setTime(time) {
    this.hostTimeSeen = true;
    this.hostTime = time;
  }

  now() {
    // steady_clock: performance.now() is monotonic.
    const wallNow = performance.now() / 1000;
    if (this.wallStart < 0.0) this.wallStart = wallNow;

    if (!this.hostTimeSeen || this.hostTime < 0.0) return wallNow - this.wallStart;

    const raw = this.hostTime;
    if (this.clockScale === 0.0 && this.lastRawTime >= 0.0 && this.lastWallTime >= 0.0) {
      const hostDelta = raw - this.lastRawTime;
      const wallDelta = wallNow - this.lastWallTime;
      if (hostDelta > 0.0 && wallDelta >= 0.0005) {
        const ratio = hostDelta / wallDelta;
        if (ratio > 0.1 && ratio < 10.0) this.secondsVotes += 1;
        else if (ratio > 100.0 && ratio < 10000.0) this.millisVotes += 1;
        if (this.secondsVotes >= kClockVotes || this.millisVotes >= kClockVotes) {
          this.clockScale = this.millisVotes > this.secondsVotes ? 0.001 : 1.0;
        }
      }
    }
    this.lastRawTime = raw;
    this.lastWallTime = wallNow;
    return this.clockScale !== 0.0 ? raw * this.clockScale : wallNow - this.wallStart;
  }
}

//---------------------------------------------------------------------------
// What the stats line under the canvas reports. It measures nothing; it says
// what the controls resolved to, which is the only way a visitor can see that
// Audio Fade has no audio to act on.
//---------------------------------------------------------------------------
const telemetry = { field: 0, cnrDb: 0, demodulator: 0, voted: false };

//---------------------------------------------------------------------------
// The renderer: Downlink::ProcessOpenGL, pass for pass.
//
//   1. resample   the clip to 1842 × 576, straight colour
//   2. encode     PAL (or component) in volts, porches at blanking, less rest
//   3. preemph    F.405, causal, at the link rate
//   4. link       the carrier, the channel and the discriminator, 501 × 576
//   5. detect     the video lowpass, every second sample, 1001 × 576
//   6. deemph     F.405's inverse at the video rate, rest put back
//   7. porch      each row's demodulated back porch, 1 × 576
//   8. clamp      what the receiver takes off each row, 1 × 576
//   9. decode     the clamp applied, PAL (or the matrix) to RGB, 921 × 576
//  10. output     to the composition's raster, and the mix
//---------------------------------------------------------------------------
function createRenderer(gl, quad) {
  const resampleShader = new Program(gl, VERTEX, RESAMPLE, 'resample');
  const encodeShader = new Program(gl, VERTEX, ENCODE, 'encode');
  const transmitShader = new Program(gl, VERTEX, PREEMPH, 'preemph');
  const linkShader = new Program(gl, VERTEX, LINK, 'link');
  const detectShader = new Program(gl, VERTEX, DETECT, 'detect');
  const deemphShader = new Program(gl, VERTEX, DEEMPH, 'deemph');
  const porchShader = new Program(gl, VERTEX, PORCH, 'porch');
  const clampShader = new Program(gl, VERTEX, CLAMP, 'clamp');
  const decodeShader = new Program(gl, VERTEX, DECODE, 'decode');
  const outputShader = new Program(gl, VERTEX, OUTPUT, 'output');

  // The decoded picture is the one buffer read BETWEEN texels (the output
  // pass scales it to the composition bilinearly). The plugin keeps it in
  // RGBA32F with GL_LINEAR, which in WebGL2 needs OES_texture_float_linear;
  // where a browser lacks that, RGBA16F is filterable in core WebGL2 and is
  // the nearest honest substitute. The page says which it got.
  const floatLinear = gl.getExtension('OES_texture_float_linear') !== null;
  const decodedFormat = floatLinear ? gl.RGBA32F : gl.RGBA16F;
  telemetry.decodedFormat = floatLinear ? 'RGBA32F' : 'RGBA16F';

  const nearest = () => new PassBuffer(gl, { filter: 'nearest' });
  const resampled = nearest();
  const encoded = nearest();
  const transmitted = nearest();
  const linked = nearest();
  const detected = nearest();
  const video = nearest();
  const porch = nearest();
  const clamped = nearest();
  const decoded = new PassBuffer(gl, { filter: 'linear' });

  // The row table: ( history + 576 ) × 1, RG32F, nearest.
  const rowCount = link.kHistoryRows + link.kRows;
  const rowTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, rowTexture);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RG32F, rowCount, 1, 0, gl.RG, gl.FLOAT, null);
  gl.bindTexture(gl.TEXTURE_2D, null);
  const rowData = new Float32Array(rowCount * 2);
  const rows = {};

  const clock = new HostClock();
  const audioBins = new Float32Array(kAudioBins); // nothing fills them here

  // `uniformArray`: the first min( size, capacity ) values, as float.
  const uniformArray = (program, name, values, capacity) => {
    const n = Math.min(values.length, capacity, 256);
    program.setArray(name, Float32Array.from(values.slice(0, n)), 1);
  };

  const pass = (target, program, body) => {
    target.bind();
    program.use();
    body();
    quad.draw();
  };

  return {
    render({ input, params, width, height, time }) {
      const p = {};
      for (const id of Object.keys(params.values)) p[id] = f32(params.get(id));

      //------------------------------------------------------------------
      // The controls, then the clock.
      //------------------------------------------------------------------
      const r = resolve(p, audioBins);
      const component = r.composite === controls.kComponent;

      clock.setTime(time);
      const now = clock.now();

      // The field on air and every row's dispersal, in double;
      // frame-relative floats cross into GLSL.
      link.MakeRowTable(now, r.dispersal, rows);
      for (let i = 0; i < rowCount; i += 1) {
        rowData[2 * i] = rows.base[i];
        rowData[2 * i + 1] = rows.slope[i];
      }

      // The link writes four link samples a texel, one block per channel.
      const kPacked = Math.floor((link.kLinkLine + 3) / 4);

      resampled.ensure(link.kLinkActive, link.kRows, gl.RGBA32F);
      encoded.ensure(link.kLinkLine, link.kRows, gl.RGBA32F);
      transmitted.ensure(link.kLinkLine, link.kRows, gl.RGBA32F);
      linked.ensure(kPacked * (component ? 3 : 1), link.kRows, gl.RGBA32F);
      detected.ensure(link.kLine, link.kRows, gl.RGBA32F);
      video.ensure(link.kLine, link.kRows, gl.RGBA32F);
      porch.ensure(1, link.kRows, gl.RGBA32F);
      clamped.ensure(1, link.kRows, gl.RGBA32F);
      decoded.ensure(link.kActive, link.kRows, decodedFormat);

      gl.bindTexture(gl.TEXTURE_2D, rowTexture);
      gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, rowCount, 1, gl.RG, gl.FLOAT, rowData);
      gl.bindTexture(gl.TEXTURE_2D, null);

      const fieldCycles = link.SubcarrierPhaseForField(rows.field) / (2.0 * 3.14159265358979323846);
      const rowCycles = link.SubcarrierStepPerRow() / (2.0 * 3.14159265358979323846);

      gl.disable(gl.BLEND);

      // 1. resample. The kit's clip fills its texture, so MaxUV is 1.
      pass(resampled, resampleShader, () => {
        bindTexture(gl, 0, input.texture);
        resampleShader.setSampler('InputTexture', 0);
        resampleShader.set('MaxUV', 1, 1);
        resampleShader.set('InputSize', input.width, input.height);
        resampleShader.set('TargetSize', link.kLinkActive, link.kRows);
      });

      // 2. encode. The uplink's test generator is the harness's; TestMode 0.
      pass(encoded, encodeShader, () => {
        bindTexture(gl, 0, resampled.texture);
        encodeShader.setSampler('Picture', 0);
        encodeShader.setInt('ComponentMode', component ? 1 : 0);
        encodeShader.set('Rest', r.rest[0], r.rest[1], r.rest[2]);
        encodeShader.set('FieldCycles', fieldCycles);
        encodeShader.set('RowCycles', rowCycles);
        encodeShader.set('LinkCycles', link.kFsc / link.kFsLink);
        encodeShader.setInt('TestMode', 0);
        encodeShader.set('TestLevel', 0);
        encodeShader.set('TestAmp', 0);
        encodeShader.set('TestFreq0', 0);
        encodeShader.set('TestFreqStep', 0);
      });

      // 3. pre-emphasis
      pass(transmitted, transmitShader, () => {
        bindTexture(gl, 0, encoded.texture);
        transmitShader.setSampler('Encoded', 0);
        uniformArray(transmitShader, 'Pre', r.preTaps, link.kMaxPreTaps);
        transmitShader.setInt('PreCount', Math.min(r.preTaps.length, link.kMaxPreTaps));
      });

      // 4. the link. setLinkUniforms(), then the pass's own.
      pass(linked, linkShader, () => {
        bindTexture(gl, 0, transmitted.texture);
        bindTexture(gl, 1, rowTexture);
        linkShader.setSampler('Transmit', 0);
        linkShader.setSampler('RowTable', 1);
        linkShader.setInt('HistoryRows', link.kHistoryRows);
        linkShader.setInt('LinkWidth', link.kLinkLine);
        linkShader.set('Deviation', r.deviation);
        linkShader.set('RadPerMHz', (2.0 * 3.14159265358979323846 * 1e6) / r.fineRate);
        linkShader.setInt('Sub', r.sub);
        linkShader.setInt('Oversample', r.oversample);
        uniformArray(linkShader, 'NoiseTaps', r.noiseTaps, 97);
        linkShader.setInt('NoiseHalf', Math.floor(r.noiseTaps.length / 2));
        linkShader.set('NoiseSigma', r.noiseSigma);
        linkShader.setInt('Rotate', r.demodulator === controls.kThresholdExtension ? 0 : 1);
        linkShader.setUint('Seed', r.seed);
        // static_cast< uint64_t >( field ) & 0xffffffff
        linkShader.setUint('FieldLow', rows.field % 4294967296);
        linkShader.setInt('Linearise', 0);
        linkShader.setInt('FlipRotation', 0);

        linkShader.set('Deviation', r.deviation);
        linkShader.setInt('PackedWidth', kPacked);
        // Four link samples share a window, unless the window would be
        // longer than the shader's arrays.
        const half = Math.floor(r.noiseTaps.length / 2);
        let perRun = 4;
        while (perRun > 1 && perRun * r.sub + 1 + 2 * half > link.kLinkWindow) perRun = Math.floor(perRun / 2);
        linkShader.setInt('PerRun', perRun);
      });

      // 5. detect
      pass(detected, detectShader, () => {
        bindTexture(gl, 0, linked.texture);
        detectShader.setSampler('Link', 0);
        uniformArray(detectShader, 'Lpf', r.lpfTaps, 65);
        detectShader.setInt('LinkWidth', link.kLinkLine);
        detectShader.setInt('PackedWidth', kPacked);
        detectShader.setInt('Channels', component ? 3 : 1);
      });

      // 6. de-emphasis. The receiver's deviation detune is the harness's; 0.
      pass(video, deemphShader, () => {
        bindTexture(gl, 0, detected.texture);
        deemphShader.setSampler('Detected', 0);
        const rxDevDetune = 0.0;
        const de = r.deTaps.map((t) => t / (1.0 + rxDevDetune));
        uniformArray(deemphShader, 'De', de, link.kMaxDeTaps);
        deemphShader.setInt('DeCount', Math.min(de.length, link.kMaxDeTaps));
        deemphShader.set('Rest', r.rest[0], r.rest[1], r.rest[2]);
      });

      // 7. porch
      pass(porch, porchShader, () => {
        bindTexture(gl, 0, video.texture);
        porchShader.setSampler('Video', 0);
        porchShader.setInt('ClampFirst', link.kClampFirst);
        porchShader.setInt('ClampCount', link.kClampCount);
      });

      // 8. clamp
      pass(clamped, clampShader, () => {
        bindTexture(gl, 0, porch.texture);
        bindTexture(gl, 1, rowTexture);
        clampShader.setSampler('Porch', 0);
        clampShader.setSampler('RowTable', 1);
        clampShader.setInt('Mode', r.clamp);
        clampShader.setInt('HistoryRows', link.kHistoryRows);
        clampShader.set('Alpha', r.slowAlpha);
        // A blanking porch through the chain: 0 V minus rest, through the
        // emphasis networks' DC gains, plus rest.
        const preDc = r.pre ? link.PreDcGain() : 1.0;
        const deDc = r.de ? 1.0 / link.PreDcGain() : 1.0;
        const porchRest = r.rest.map((rest) => rest - deDc * preDc * rest);
        clampShader.set('PorchRest', porchRest[0], porchRest[1], porchRest[2]);
        clampShader.set('DispToVolts', deDc / r.deviation);
        clampShader.set('PorchCentre', link.kClampFirst + 0.5 * (link.kClampCount - 1));
      });

      // 9. decode
      pass(decoded, decodeShader, () => {
        bindTexture(gl, 0, video.texture);
        bindTexture(gl, 1, clamped.texture);
        decodeShader.setSampler('Video', 0);
        decodeShader.setSampler('Clamp', 1);
        decodeShader.setInt('ComponentMode', component ? 1 : 0);
        decodeShader.set('FieldCycles', fieldCycles);
        decodeShader.set('RowCycles', rowCycles);
        decodeShader.set('VideoCycles', link.kFsc / link.kFsVideo);
        decodeShader.set('LumaCutoff', 5.5e6 / link.kFsVideo);
        decodeShader.set('ChromaCutoff', 1.3e6 / link.kFsVideo);
      });

      // 10. output, to the page's canvas at the composition's size.
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);
      outputShader.use();
      bindTexture(gl, 0, input.texture);
      bindTexture(gl, 1, decoded.texture);
      outputShader.setSampler('InputTexture', 0);
      outputShader.setSampler('Decoded', 1);
      outputShader.set('MaxUV', 1, 1);
      outputShader.set('Mix', r.mix);
      quad.draw();

      // Leave no pass's texture bound, so the kit's clip pass next frame never
      // renders into a texture a unit still holds.
      bindTexture(gl, 1, null);
      bindTexture(gl, 0, null);

      telemetry.field = rows.field;
      telemetry.cnrDb = r.cnrDb;
      telemetry.demodulator = r.demodulator;
      telemetry.voted = clock.clockScale !== 0.0;
    },
  };
}

//---------------------------------------------------------------------------
// The parameters, from Downlink::Downlink(), in its order and groups.
//---------------------------------------------------------------------------

const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });

// Noise Seed: FF_TYPE_INTEGER, SetParamRange( 0, 999 ), default 1.
const SEED_ELEMENTS = Array.from({ length: 1000 }, (_, i) => String(i));

const demo = mountDemo({
  name: 'Downlink',
  pluginId: 'DL01',
  tagline:
    'Analogue FM satellite television. The clip is encoded to PAL, pre-emphasised, and frequency-modulated onto a carrier swept by the 25 Hz energy-dispersal triangle; the carrier crosses a noisy link into a Gaussian IF and an FM discriminator, then de-emphasis, a video lowpass, a clamp and the PAL decoder. Nothing is painted on: above threshold the FM noise lands on the colour subcarrier as fine coloured grain, below it the discriminator clicks and the clicks are the white and black sparklies, and with a bad clamp the dispersal rocks the picture at the field rate.',
  repo: 'https://github.com/stoatworks-labs/downlink',
  blurb:
    'It runs Downlink’s own ten GLSL passes, copied unedited into WebGL2, with the link’s CPU half — the filter design, the noise statistics, the per-row dispersal table and the clock — ported to JavaScript, on generated clips in this page. There is no audio in a browser, so Audio Fade is here but has nothing to act on.',

  // The output's alpha is the clip's, and the received colour is multiplied by
  // it, so on the transparent clip what is behind is a real question.
  showBackdrop: true,

  // Every signal buffer is RGBA32F, as in the plugin: the chain is in volts,
  // and eight bits between the discriminator and the de-emphasis would be a
  // quantiser deciding how big a click is.
  needFloat: true,

  params: [
    std('deviation', 'Deviation', controls.DeviationParam(13.5), 'Uplink', {
      display: (v) => `${controls.DeviationMHzPerVolt(v).toFixed(1)} MHz/V`,
      hint: 'Peak-to-peak MHz for 1 V at the pre-emphasis crossover, 4 to 24. 13.5 is Intelsat’s figure, Astra used 16. More deviation is a bigger FM improvement: a cleaner picture at the same CNR.',
    }),
    std('dispersal', 'Dispersal', controls.DispersalParam(2.0), 'Uplink', {
      display: (v) => `${controls.DispersalMHz(v).toFixed(2)} MHz p-p`,
      hint: 'The 25 Hz energy-dispersal triangle, 0 to 8 MHz peak to peak. A good clamp takes it back off the picture; with Clamp Slow or Off it comes through as a flicker.',
    }),
    bool('preEmphasis', 'Pre-emphasis On', 1, 'Uplink',
      'ITU-R F.405 (CCIR 405), 625 lines: the lows down 11 dB before the carrier. It has to match De-emphasis On at the receiver — turn one off and not the other to see the mismatch.'),

    std('cnr', 'CNR', controls.CnrParam(9.0), 'Link', {
      display: (v) => `${controls.CnrDb(v).toFixed(1)} dB`,
      hint: 'Carrier-to-noise in the IF, 0 to 25 dB. The main control. The knee is near 8.7 dB with the Discriminator: above it, fine grain; around it, a sprinkle of sparklies; below it, a storm.',
    }),
    std('rain', 'Rain Fade', 0.0, 'Link', {
      display: (v) => `${controls.RainFadeDb(v) > 0 ? '−' : ''}${controls.RainFadeDb(v).toFixed(1)} dB`,
      hint: '0 to 10 dB taken off the CNR, so a fade can be ridden without losing where the link was set.',
    }),
    std('audioFade', 'Audio Fade', 0.0, 'Link', {
      // AudioFadeDb( p, level ) is 0 dB at the only level this page has, whatever
      // the slider says; the readout says why rather than showing a dead 0.0.
      display: () => 'no audio',
      hint: 'NO AUDIO IN THE BROWSER. In Resolume this takes up to 12 dB off the CNR times the RMS level of the audio the host feeds the effect. There is no host here to feed it, so the level is 0 and this slider changes nothing — it is on the panel because the plugin declares it.',
    }),
    std('ifBandwidth', 'IF Bandwidth', controls.IfBandwidthParam(27.0), 'Link', {
      display: (v) => `${controls.IfBandwidthMHz(v).toFixed(1)} MHz`,
      hint: 'The −3 dB width of the Gaussian IF, 18 to 36 MHz; 27 is a full transponder. CNR is measured across the IF, so this spreads the same noise rather than adding any.',
    }),
    opt('seed', 'Noise Seed', SEED_ELEMENTS, 1, 'Link',
      'FF_TYPE_INTEGER, 0 to 999, a dropdown here because the page has no integer control. The noise is a function of the seed and the field only.'),

    opt('demodulator', 'Demodulator', controls.DemodulatorNames, controls.kDiscriminator, 'Receiver',
      'Discriminator: a plain FM discriminator sampled at 283.75 MHz, knee near 8.7 dB. Threshold Ext.: an ideal tracking filter 3 dB narrower in noise, knee near 4.9 dB. Try it at a CNR of about 6.5 dB.'),
    opt('clamp', 'Clamp', controls.ClampNames, controls.kClampGood, 'Receiver',
      'Good: a keyed back-porch clamp, per line. Slow: a DC restorer with a 2 ms time constant. Off: no clamp. SLOW AND OFF FLICKER — the 25 Hz dispersal comes through as a large-area flash at the field rate.'),
    bool('deEmphasis', 'De-emphasis On', 1, 'Receiver',
      'F.405’s inverse at the receiver. See Pre-emphasis On for the four combinations.'),
    std('videoBandwidth', 'Video Bandwidth', controls.VideoBandwidthParam(5.0), 'Receiver', {
      display: (v) => `${controls.VideoBandwidthMHz(v).toFixed(2)} MHz`,
      hint: 'The video lowpass after the discriminator, 2 to 6 MHz. In PAL the colour rides a 4.43 MHz subcarrier, so below about 4.5 MHz the colour goes.',
    }),

    opt('composite', 'Composite', controls.CompositeNames, controls.kPal, 'Signal',
      'PAL: one carrier with a colour subcarrier, the noise landing on it as coloured grain. Component: Y, U and V on three carriers of their own, an idealised MAC — sharper, cleaner colour, coloured sparklies, three times the link.'),
    std('mix', 'Mix', 1.0, 'Signal', {
      hint: 'The received picture against the untouched clip. The alpha is always the clip’s.',
    }),
  ],

  // Colour bars first: the classic thing to put up a satellite link, and the
  // one clip whose right answer is known. Lights on black shows the white
  // sparklies, a bright ramp the dark ones.
  sources: ['bars', 'scene', 'spot', 'ramp', 'detail', 'grid', 'alpha'],

  // The plugin ships no factory presets, so these are the page's own —
  // expressed entirely in the plugin's parameters and reachable with the
  // sliders.
  presets: {
    'Clean link (14 dB)': { cnr: controls.CnrParam(14.0) },
    'At the knee (8 dB)': { cnr: controls.CnrParam(8.0) },
    'Sparklies (7 dB)': { cnr: controls.CnrParam(7.0) },
    'Storm (5 dB)': { cnr: controls.CnrParam(5.0) },
    'Threshold Ext. at 6.5 dB': { cnr: controls.CnrParam(6.5), demodulator: controls.kThresholdExtension },
    'Discriminator at 6.5 dB': { cnr: controls.CnrParam(6.5) },
    'Component (MAC-style), 7 dB': { composite: controls.kComponent, cnr: controls.CnrParam(7.0) },
    'Pre-emphasis off, De-emphasis on': { preEmphasis: 0 },
    'Pre-emphasis on, De-emphasis off': { deEmphasis: 0 },
    'Clamp Slow — flickers': { clamp: controls.kClampSlow },
    'Clamp Off — flickers hard': { clamp: controls.kClampOff },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Link.cpp (the F.405 networks, the video lowpass, the Gaussian IF’s taps, the dispersal triangle, the per-row table, the field index, the subcarrier phase), the conversions in Controls.h, and what Downlink.cpp does every frame (resolve(), the clock’s unit voting, every uniform) are ported to JavaScript function for function, in double where the C++ is double and rounded to float where it is float. JavaScript’s Math.exp, sin and pow are the browser’s, not the C library’s, and may differ in the last bit. Nothing checks a port but a reader; the repository’s dltest checks the C++ and has never heard of this page.',
    'The GPU passes are not a port. Resample, encode, pre-emphasis, the link, detect, de-emphasis, porch, clamp, decode and output are the plugin’s own GLSL, and demo/tools/check_shaders.py fails the repository’s verify script if a character of any of the twelve pieces drifts. The link runs at the plugin’s own raster — 2002 × 576 link samples, the discriminator at 283.75 MHz — whatever the composition size, so on a weak GPU this page is slow rather than wrong.',
    'There is no audio in a browser. The plugin declares an Audio buffer parameter, 64 FFT bins that Resolume fills every frame, and Audio Fade takes up to 12 dB off the link times their RMS level. Here nothing fills the bins, the level is 0, and Audio Fade — on the panel because the plugin declares it — changes nothing. The Audio buffer itself is not on the panel: it is not a control anybody sets, and the kit has none to show it with. The effective CNR on this page is CNR minus Rain Fade.',
    'Noise Seed is FF_TYPE_INTEGER, 0 to 999, in the plugin. The demo kit has no integer control, so it is a dropdown of all thousand values.',
    'The page’s clock stands in for the host’s. The plugin shows the PAL field on air at the host’s time, one field every 20 ms, and votes for four frames on whether the host counts in seconds or milliseconds, running on its own steady clock meanwhile; that vote is ported and settles on seconds, since the page’s clock is in seconds. The page draws at your display’s refresh rate, so at 60 Hz one frame in six repeats the last field’s noise, exactly as the plugin does in a 60 fps composition.',
    'Every signal buffer is RGBA32F, as in the plugin, which needs EXT_color_buffer_float. The decoded picture, the one buffer read between texels, is RGBA32F where the browser can filter float textures (OES_texture_float_linear) and RGBA16F where it cannot; the line under the canvas says which you got.',
    'The harness’s test hooks and its link probe (kLinkProbeShader) are not here. They are always off in the plugin and exist so dltest can break the model on purpose; the page sets their inert values, as the plugin does.',
    'Clamp Slow and Clamp Off flicker by design: the 25 Hz dispersal triangle comes through as a large-area flash at the field rate. Treat them as a strobe.',
    'The presets are the page’s own. The plugin ships none; each one here is only a setting of the plugin’s own parameters.',
    'The plugin’s numerical proof — F.405 met to 0.02 dB, click counts against the sampled discriminator’s exact rate, SNR against the FM improvement, the dispersal through each clamp per pixel — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});

//---------------------------------------------------------------------------
// The line under the canvas.
//
// Not a measurement: what the controls resolved to this frame. It is the one
// place the page can show that Audio Fade has nothing to act on, and where the
// link stands against the knee the plugin's harness measured. Skipped in embed
// mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    line.dataset.role = 'link-state';
    stage.append(line);

    setInterval(() => {
      const { field, cnrDb, demodulator, voted, decodedFormat } = telemetry;
      // The knees are the harness's measurements (AGENTS.md), quoted, not
      // computed here.
      const knee = demodulator === controls.kThresholdExtension ? 4.9 : 8.7;
      const where = cnrDb > knee + 2 ? 'above threshold: grain'
        : cnrDb > knee - 1 ? 'around the knee: sparklies'
          : 'below threshold: clicks everywhere';
      line.textContent =
        `Field ${field.toLocaleString('en-GB')} on air${voted ? '' : ' (the clock is still voting)'}. `
        + `Link ${cnrDb.toFixed(1)} dB = CNR − Rain Fade − Audio Fade at 0 audio; `
        + `the harness put this demodulator’s knee near ${knee} dB, so ${where}. `
        + `Decoded picture in ${decodedFormat}.`;
    }, 250);
  }
}
