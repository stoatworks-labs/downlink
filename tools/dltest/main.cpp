/**
	dltest -- render Downlink offline, and measure what its link does.

	Every check drives the REAL plugin class in a headless GL 4.1 context on a
	synthetic 60 fps clock, reads back what it drew -- the picture, or one of
	the signal buffers, or the discriminator's own fine-rate output through the
	probe that runs the plugin's link library -- and measures it against a
	closed form. See AGENTS.md for where each tolerance comes from.

		dltest --out /tmp/frame.png     a picture, on the test card
		dltest --list                   every parameter, its kind, default and range
		dltest --names                  no display name the host would truncate
		dltest --clock                  a six-day host clock makes the same rows
		dltest --emphasis               F.405 met; pre- then de-emphasis flat
		dltest --clicks                 click rate against Rice's formula, 4 CNRs
		dltest --polarity               more negative clicks near white, by Rice's
		                                modulation term
		dltest --threshold              SNR = CNR + the FM improvement, 1 dB/dB,
		                                then the knee; threshold extension moves it
		dltest --dispersal              the 25 Hz triangle with Clamp Off; Slow's
		                                lag; Good's per-line bound
		dltest --identity               a clean link returns the input
		dltest --seed                   bit-identical noise for a seed and field,
		                                at both rasters
		dltest --resize                 a resize mid-run changes nothing after
		dltest --negative               every check above FAILS on a broken model
		dltest --offline                every check that needs no GL
		dltest --bench                  the render cost
		dltest --pipe                   raw RGBA frames in, raw RGBA frames out
		dltest --dump-shaders DIR       every shader the plugin compiles, as files

	`--pipe` takes the fleet's frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | dltest --pipe --size 1920x1080 [--script cues.txt] [--audio 0.5] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Downlink.h"
#include "Link.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace downlink;

namespace
{
constexpr double kPi  = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

int g_checks   = 0;
int g_failures = 0;
bool g_quiet   = false;

/// One assertion. Returns whether it held.
bool check( bool ok, const char* format, ... ) __attribute__( ( format( printf, 2, 3 ) ) );
bool check( bool ok, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	if( !g_quiet || !ok )
	{
		std::printf( "   %s  ", ok ? "ok  " : "FAIL" );
		va_list args;
		va_start( args, format );
		std::vprintf( format, args );
		va_end( args );
		std::printf( "\n" );
	}
	return ok;
}

void note( const char* format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
void note( const char* format, ... )
{
	if( g_quiet )
		return;
	std::printf( "         " );
	va_list args;
	va_start( args, format );
	std::vprintf( format, args );
	va_end( args );
	std::printf( "\n" );
}

void heading( const char* text )
{
	if( !g_quiet )
		std::printf( "%s\n", text );
}

//---------------------------------------------------------------------------
// PNG
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	for( int s = 24; s >= 0; s -= 8 )
		out.push_back( static_cast< unsigned char >( value >> s ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf size = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( size );
	if( compress2( compressed.data(), &size, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( size );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { 8, 6, 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources, top row first.
//---------------------------------------------------------------------------
using Image = std::vector< unsigned char >;

void putPixel( Image& image, int width, int x, int y, float r, float g, float b )
{
	const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
	image[ i + 0 ] = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
	image[ i + 1 ] = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
	image[ i + 2 ] = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
	image[ i + 3 ] = 255;
}

/// 75% colour bars over the top half, a luma ramp, a black field with a white
/// box (white on black shows the sparklies of both polarities where each is
/// likelier), and a grey surround.
Image buildCard( int width, int height )
{
	Image card( static_cast< size_t >( width ) * height * 4 );
	static const float bars[ 8 ][ 3 ] = { { 0.75f, 0.75f, 0.75f }, { 0.75f, 0.75f, 0.0f }, { 0.0f, 0.75f, 0.75f },
		                                  { 0.0f, 0.75f, 0.0f },   { 0.75f, 0.0f, 0.75f }, { 0.75f, 0.0f, 0.0f },
		                                  { 0.0f, 0.0f, 0.75f },   { 0.0f, 0.0f, 0.0f } };
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const float u = ( x + 0.5f ) / width;
			const float v = ( y + 0.5f ) / height;
			float r = 0.5f, g = 0.5f, b = 0.5f;
			if( v < 0.5f )
			{
				const int bar = std::min( 7, static_cast< int >( u * 8.0f ) );
				r             = bars[ bar ][ 0 ];
				g             = bars[ bar ][ 1 ];
				b             = bars[ bar ][ 2 ];
			}
			else if( v < 0.65f )
				r = g = b = u;
			else
			{
				r = g = b = 0.02f;
				if( u > 0.30f && u < 0.70f && v > 0.72f && v < 0.93f )
					r = g = b = 1.0f;
			}
			putPixel( card, width, x, y, r, g, b );
		}
	return card;
}

Image buildFlat( int width, int height, int code )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	const float v = static_cast< float >( code ) / 255.0f;
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
			putPixel( image, width, x, y, v, v, v );
	return image;
}

/// Four saturated quadrants, for the identity check.
struct Quadrant
{
	float rgb[ 3 ];
};
const Quadrant kQuadrants[ 4 ] = { { { 0.80f, 0.20f, 0.10f } }, { { 0.10f, 0.70f, 0.30f } },
	                               { { 0.20f, 0.30f, 0.85f } }, { { 0.90f, 0.85f, 0.15f } } };
Image buildQuadrants( int width, int height )
{
	Image image( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const int q      = ( y * 2 >= height ? 2 : 0 ) + ( x * 2 >= width ? 1 : 0 );
			const float* rgb = kQuadrants[ q ].rgb;
			putPixel( image, width, x, y, rgb[ 0 ], rgb[ 1 ], rgb[ 2 ] );
		}
	return image;
}

//---------------------------------------------------------------------------
// GL
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = { kCGLPFAOpenGLProfile,
		                                            static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		                                            kCGLPFAAccelerated,
		                                            kCGLPFAColorSize,
		                                            static_cast< CGLPixelFormatAttribute >( 24 ),
		                                            kCGLPFAAlphaSize,
		                                            static_cast< CGLPixelFormatAttribute >( 8 ),
		                                            static_cast< CGLPixelFormatAttribute >( 0 ) };
	const CGLPixelFormatAttribute software[]    = { kCGLPFAOpenGLProfile,
		                                            static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		                                            kCGLPFAColorSize,
		                                            static_cast< CGLPixelFormatAttribute >( 24 ),
		                                            kCGLPFAAlphaSize,
		                                            static_cast< CGLPixelFormatAttribute >( 8 ),
		                                            static_cast< CGLPixelFormatAttribute >( 0 ) };
	CGLPixelFormatObj format = nullptr;
	GLint count              = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &count ) != kCGLNoError || format == nullptr )
		if( CGLChoosePixelFormat( software, &format, &count ) != kCGLNoError || format == nullptr )
			return nullptr;
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, nullptr );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value, low, high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Downlink::DL_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Downlink& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Downlink::DL_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p{ name ? name : "?", i, plugin.GetParamType( i ), plugin.GetFloatParameter( i ), 0.0f, 1.0f };
		//An option's range reads back 0..1 whatever its element count, so the
		//element count is the range.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Downlink& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Downlink& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const int index = indexOfParameter( plugin, assignment.substr( 0, equals ) );
	if( index < 0 )
	{
		error = "no parameter called '" + assignment.substr( 0, equals ) + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ),
	                          std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

void set( Downlink& plugin, unsigned int id, float value )
{
	plugin.SetFloatParameter( id, value );
}

/// A synthetic spectrum, level L: a falling slope with a kick on the beat.
void injectSpectrum( Downlink& plugin, float level, double seconds )
{
	const float kick = static_cast< float >( std::exp( -6.0 * std::fmod( seconds, 0.5 ) ) );
	for( int i = 0; i < Downlink::kAudioBins; ++i )
	{
		const float t = static_cast< float >( i ) / 63.0f;
		float value   = 0.6f * std::exp( -3.2f * t ) + ( i < 8 ? kick : 0.0f );
		plugin.SetParamElementValue( Downlink::DL_AUDIO, static_cast< unsigned int >( i ),
		                             std::clamp( value * level, 0.0f, 1.0f ) );
	}
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock.
//---------------------------------------------------------------------------
struct Session
{
	std::unique_ptr< Downlink > owned = std::make_unique< Downlink >();
	Downlink& plugin                  = *owned;
	int width = 0, height = 0;
	bool floatOutput     = false;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	bool begin( int w, int h )
	{
		width                       = w;
		height                      = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		allocate();
		return true;
	}

	void allocate()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		sourceTexture = makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		process.HostFBO                                 = outputFBO;
	}

	/// A new raster mid-run, as a host resizing its composition does.
	void resize( int w, int h )
	{
		width  = w;
		height = h;
		allocate();
	}

	bool renderAt( double seconds, const Image& pixels )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( seconds );
		const Image flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed at t=%.6f\n", seconds );
		return ok;
	}

	bool render( int frame, const Image& pixels )
	{
		return renderAt( frame / 60.0, pixels );
	}

	Image readBack()
	{
		Image pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// A signal buffer; texel row = scan row, so NO flip.
	std::vector< float > buffer( Downlink::Buffer which, int& w, int& h )
	{
		std::vector< float > out;
		plugin.ReadBufferForTest( which, out, w, h );
		return out;
	}

	void end()
	{
		plugin.DeInitGL();
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}
};

/// The time, in seconds, of the start of field `field` plus a millisecond:
/// comfortably inside it, so the field index is not a floor of a boundary.
double fieldTime( int64_t field )
{
	return field * link::kFieldSeconds + 0.001;
}

//---------------------------------------------------------------------------
// Failure bookkeeping for --negative: each case names the check that must be
// among the failures.
//---------------------------------------------------------------------------
std::vector< std::string > g_failed;

bool checkNamed( const std::string& label, bool ok, const std::string& detail )
{
	if( !ok )
		g_failed.push_back( label );
	return check( ok, "%s: %s", label.c_str(), detail.c_str() );
}

std::string fmt( const char* format, ... ) __attribute__( ( format( printf, 1, 2 ) ) );
std::string fmt( const char* format, ... )
{
	char buffer[ 1024 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

/// The model perturbations --negative applies. All off is the plugin.
struct Hooks
{
	bool linearise   = false;
	bool flip        = false;
	bool skipTau     = false;
	double deDetune  = 0.0;
	double rxDetune  = 0.0;
	bool floatClock  = false;//harness-side: the trap --clock guards against
	double pe405Detune = 0.0;//harness-side: a network that misses F.405
};

void applyHooks( Downlink& plugin, const Hooks& h )
{
	plugin.SetLineariseForTest( h.linearise );
	plugin.SetFlipRotationForTest( h.flip );
	plugin.SetSkipClampTauForTest( h.skipTau );
	plugin.SetDeDetuneForTest( h.deDetune );
	plugin.SetReceiverDeviationDetuneForTest( h.rxDetune );
}

//---------------------------------------------------------------------------
// Standard normal helpers.
//---------------------------------------------------------------------------
double phi( double x )
{
	return std::exp( -0.5 * x * x ) / std::sqrt( kTau );
}
double Phi( double x )
{
	return 0.5 * std::erfc( -x / std::sqrt( 2.0 ) );
}

//---------------------------------------------------------------------------
// The noise the discriminator sees, as a spectrum. The carrier-frame noise is
// m[ k ] = sigma sum h[ i ] u[ k-i ] exp( -j i dphi0 ) for a carrier sitting
// dphi0 rad per fine sample off the IF's centre, so its PSD is the taps'
// |H|^2 shifted to -df. The moments are Rice's b0, b1, b2 (b1, b2 in rad/s).
// Taken by numerical integration over the fine rate's Nyquist band; 32768
// points on a spectrum that is a smooth Gaussian is exact to double's noise.
//---------------------------------------------------------------------------
struct Moments
{
	double b0 = 0, b1 = 0, b2 = 0;
	double rmsHz() const
	{
		return std::sqrt( b2 / b0 - ( b1 / b0 ) * ( b1 / b0 ) ) / kTau;
	}
};

Moments noiseMoments( const std::vector< double >& taps, double sigma, double fs, double dphi0 )
{
	const int half = static_cast< int >( taps.size() / 2 );
	const int n    = 32768;
	Moments m;
	for( int k = 0; k < n; ++k )
	{
		const double nu = ( ( k + 0.5 ) / n - 0.5 ) * fs;
		double re = 0.0, im = 0.0;
		for( int i = -half; i <= half; ++i )
		{
			const double a = -i * dphi0 - kTau * nu * i / fs;
			re += taps[ static_cast< size_t >( i + half ) ] * std::cos( a );
			im += taps[ static_cast< size_t >( i + half ) ] * std::sin( a );
		}
		const double S = sigma * sigma * ( re * re + im * im ) / fs;//per Hz
		const double w = kTau * nu;
		const double dn = fs / n;
		m.b0 += S * dn;
		m.b1 += w * S * dn;
		m.b2 += w * w * S * dn;
	}
	return m;
}

//---------------------------------------------------------------------------
// Rice, exactly: the rate the phasor 1 + m crosses the negative real axis in
// each direction. A crossing with y falling is the phase rising through pi, a
// positive click. y is independent of ( x, y' ); y' given x is Gaussian with
// mean ( b1 / b0 ) x and variance ( b2 - b1^2 / b0 ) / 2. At b1 = 0 this is
// r erfc( sqrt( rho ) ), which --offline checks.
//---------------------------------------------------------------------------
struct Rates
{
	double plus = 0, minus = 0;
	double total() const
	{
		return plus + minus;
	}
};

Rates riceRates( const Moments& m )
{
	const double sx  = std::sqrt( m.b0 / 2.0 );
	const double k   = m.b1 / m.b0;
	const double sd  = std::sqrt( std::max( 1e-300, ( m.b2 - m.b1 * m.b1 / m.b0 ) / 2.0 ) );
	const double py0 = 1.0 / std::sqrt( kPi * m.b0 );
	const double lo  = -1.0 - 14.0 * sx;
	const int n      = 20000;//Simpson, even
	const double h   = ( -1.0 - lo ) / n;
	Rates r;
	for( int i = 0; i <= n; ++i )
	{
		const double x  = lo + i * h;
		const double w  = ( i == 0 || i == n ) ? 1.0 : ( i % 2 ? 4.0 : 2.0 );
		const double px = std::exp( -x * x / ( 2.0 * sx * sx ) ) / ( std::sqrt( kTau ) * sx );
		const double mu = k * x;
		r.plus += w * px * ( sd * phi( mu / sd ) - mu * Phi( -mu / sd ) );
		r.minus += w * px * ( sd * phi( mu / sd ) + mu * Phi( mu / sd ) );
	}
	r.plus *= py0 * h / 3.0;
	r.minus *= py0 * h / 3.0;
	return r;
}

/// The textbook's form: r erfc( sqrt rho ) + |df| exp( -rho ), the second
/// term all of the sign that pulls the frequency back to the IF's centre.
Rates riceTextbook( double rmsHz, double rho, double dfHz )
{
	Rates r;
	r.plus  = 0.5 * rmsHz * std::erfc( std::sqrt( rho ) );
	r.minus = r.plus;
	( dfHz >= 0 ? r.minus : r.plus ) += std::abs( dfHz ) * std::exp( -rho );
	return r;
}

//---------------------------------------------------------------------------
// The discriminator the plugin runs is sampled, so its clicks are not quite
// Rice's: it slips at fine sample k exactly when
//
//     | dphi + psi[ k ] - psi[ k-1 ] | > pi,
//
// a sign each way. That probability is exact and computable. m[ k-1 ] is
// circular Gaussian of variance sigma^2; given it, m[ k ] is circular
// Gaussian with mean c m[ k-1 ] and variance sigma^2 ( 1 - |c|^2 ), c the
// lag-one correlation of the carrier-frame taps; and the phase of a circular
// Gaussian about a mean has a closed-form density (the Rician phase law). So
// the rate is a 2-D integral over m[ k-1 ] of that density over the arc that
// slips, times the fine rate. No simulation, no second discriminator.
//---------------------------------------------------------------------------
double ricianPhaseDensity( double psi, double a, double theta, double s )
{
	const double beta = a * std::cos( psi - theta ) / s;
	const double sin2 = std::sin( psi - theta );
	const double e0   = std::exp( -a * a / ( s * s ) );
	const double e1   = std::exp( -a * a * sin2 * sin2 / ( s * s ) );
	return ( e0 + std::sqrt( kPi ) * beta * e1 * ( 1.0 + std::erf( beta ) ) ) / kTau;
}

double arcProbability( double lo, double hi, double a, double theta, double s, int n = 64 )
{
	if( hi <= lo )
		return 0.0;
	const double h = ( hi - lo ) / n;
	double sum     = 0.0;
	for( int i = 0; i <= n; ++i )
		sum += ( i == 0 || i == n ? 1.0 : ( i % 2 ? 4.0 : 2.0 ) ) * ricianPhaseDensity( lo + i * h, a, theta, s );
	return sum * h / 3.0;
}

Rates discreteRates( const std::vector< double >& taps, double sigma, double fs, double dphi0, double dz = 0.025,
                     int arcPoints = 64 )
{
	const int half = static_cast< int >( taps.size() / 2 );
	double lag1    = 0.0;
	for( int i = -half; i < half; ++i )
		lag1 += taps[ static_cast< size_t >( i + 1 + half ) ] * taps[ static_cast< size_t >( i + half ) ];
	//c = exp( -j dphi0 ) sum h[ i+1 ] h[ i ]: the rotation into the carrier's
	//frame turns the taps by exp( -j i dphi0 ).
	const double cr = lag1 * std::cos( dphi0 ), ci = -lag1 * std::sin( dphi0 );
	const double s  = sigma * std::sqrt( std::max( 1e-300, 1.0 - lag1 * lag1 ) );
	const double L  = 7.0;
	const int n     = static_cast< int >( 2.0 * L / dz );
	double pPlus = 0.0, pMinus = 0.0;
	//Cell centres, never a grid point ON the axis: psi jumps by 2 pi across
	//the negative real axis, and a row of points on it would all be counted
	//on the upper side, every one a positive click (this first version did
	//exactly that, and was 10% out on the split while right on the total).
	for( int ix = 0; ix < n; ++ix )
		for( int iy = 0; iy < n; ++iy )
		{
			const double zx = -L + ( ix + 0.5 ) * dz, zy = -L + ( iy + 0.5 ) * dz;
			const double w  = std::exp( -( zx * zx + zy * zy ) ) / kPi * dz * dz;
			if( w < 1e-30 )
				continue;
			const double mx = sigma * zx, my = sigma * zy;
			const double psi1 = std::atan2( my, 1.0 + mx );
			const double ux = 1.0 + cr * mx - ci * my, uy = ci * mx + cr * my;
			const double a = std::hypot( ux, uy ), theta = std::atan2( uy, ux );
			//Negative: psi2 > pi - dphi0 + psi1. Positive: psi2 < -pi - dphi0 + psi1.
			pMinus += w * arcProbability( kPi - dphi0 + psi1, kPi, a, theta, s, arcPoints );
			pPlus += w * arcProbability( -kPi, -kPi - dphi0 + psi1, a, theta, s, arcPoints );
		}
	Rates r;
	r.plus  = pPlus * fs;
	r.minus = pMinus * fs;
	return r;
}

//---------------------------------------------------------------------------
// Counting clicks in the discriminator's output. The probe gives, per fine
// sample, the discriminator output d, the phase increment the uplink sent,
// and (used once, at the row's first sample) the phase error psi. The phase
// error then runs as Psi = psi0 + sum( d - dphi ), which is psi + 2 pi C for
// the whole number C of cycles slipped; every change of C is a click, of its
// sign.
//---------------------------------------------------------------------------
struct Count
{
	long plus = 0, minus = 0;
	double seconds = 0;
	std::vector< long > perRow;
	double dphiSum = 0;
	long samples   = 0;
};

void countClicks( const std::vector< float >& p, int rows, int sub, double fineRate, Count& c )
{
	const int W = link::kLinkLine;
	for( int r = 0; r < rows; ++r )
	{
		double Psi = 0.0;
		long C     = 0;
		long n     = 0;
		bool first = true;
		for( int o = 0; o < W; ++o )
			for( int s = 0; s < sub; ++s )
			{
				const size_t i  = ( ( static_cast< size_t >( r ) * sub + s ) * W + o ) * 4;
				const double d  = p[ i ], dphi = p[ i + 1 ], psi = p[ i + 2 ];
				c.dphiSum += dphi;
				++c.samples;
				if( first )
				{
					Psi   = psi;
					first = false;
					continue;
				}
				Psi += d - dphi;
				const long Cn = std::lround( Psi / kTau );
				if( Cn > C )
					c.plus += Cn - C, n += Cn - C;
				else if( Cn < C )
					c.minus += C - Cn, n += C - Cn;
				C = Cn;
			}
		c.perRow.push_back( n );
		c.seconds += ( static_cast< double >( W ) * sub - 1.0 ) / fineRate;
	}
}

/// A session for a signal check: the uplink's test generator on, the clip a
/// flat grey (it is not read), everything else at the plugin's defaults.
struct SignalRig
{
	Session session;
	Image clip;
	bool begin( int width, int height, const Hooks& hooks )
	{
		applyHooks( session.plugin, hooks );
		clip = buildFlat( width, height, 128 );
		return session.begin( width, height );
	}
	void setPhysical( unsigned int id, float value )
	{
		session.plugin.SetFloatParameter( id, value );
	}
	bool field( int64_t f )
	{
		return session.renderAt( fieldTime( f ), clip );
	}
};

/// Probe every row of the frame last rendered, 64 at a time.
bool probeFrame( Downlink& plugin, int channel, Count& c )
{
	const auto& r = plugin.ResolvedForTest();
	std::vector< float > p;
	for( int row0 = 0; row0 < link::kRows; row0 += 64 )
	{
		const int n = std::min( 64, link::kRows - row0 );
		if( !plugin.ProbeForTest( row0, n, channel, p ) )
			return false;
		countClicks( p, n, r.sub, r.fineRate, c );
	}
	return true;
}

//---------------------------------------------------------------------------
// --clicks
//
// An unmodulated carrier: the uplink sends its rest level on every sample, so
// the pre-emphasis passes exactly zero, and the dispersal is off. The counted
// rate against Rice's for the IF these taps actually make, within a Poisson
// interval of 4 sigma on the expected count. Then the same count at four times
// the discriminator's rate, which is how the 16x sampling is shown to find the
// continuous-time crossings Rice counted.
//---------------------------------------------------------------------------
int runClicks( int width, int height, const Hooks& hooks = {} )
{
	heading( "--clicks: the click rate follows Rice's formula" );
	SignalRig rig;
	if( !rig.begin( width, height, hooks ) )
		return 1;
	Downlink& p = rig.session.plugin;
	p.SetTestSignalForTest( 1, static_cast< float >( link::kRestVolts ) );
	p.SetFloatParameter( Downlink::DL_DISPERSAL, 0.0f );

	const int before  = g_failures;
	const double cnrs[] = { 4.0, 6.0, 8.0, 10.0 };
	int64_t field     = 1000;
	for( double cnr : cnrs )
	{
		p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( cnr ) ) );
		rig.field( field );
		const auto& r     = p.ResolvedForTest();
		const Moments m   = noiseMoments( r.noiseTaps, r.noiseSigma, r.fineRate, 0.0 );
		const Rates rice  = riceRates( m );
		const double rho  = 1.0 / m.b0;

		Count c;
		int fields = 0;
		//Enough fields for 2000 expected clicks, at most 100: at 10 dB that is
		//about 290, and the interval says so.
		while( true )
		{
			if( !probeFrame( p, 0, c ) )
				return 1;
			++fields;
			if( rice.total() * c.seconds >= 2000.0 || fields >= 100 )
				break;
			rig.field( ++field );
		}
		++field;
		//The exact rate, on two grids: the integral converges as the grid's
		//square, so its error on the finer is a third of their difference.
		const Rates coarse    = discreteRates( r.noiseTaps, r.noiseSigma, r.fineRate, 0.0, 0.025 );
		const Rates exact     = discreteRates( r.noiseTaps, r.noiseSigma, r.fineRate, 0.0, 0.0125 );
		const double expected = exact.total() * c.seconds;
		const double numeric  = std::abs( exact.total() - coarse.total() ) / 3.0 * c.seconds;
		const long got        = c.plus + c.minus;
		const double tol      = 4.0 * std::sqrt( std::max( expected, 1.0 ) ) + numeric;
		//Dispersion across rows: a Poisson count has variance = mean.
		double mean = 0, var = 0;
		for( long v : c.perRow )
			mean += v;
		mean /= c.perRow.size();
		for( long v : c.perRow )
			var += ( v - mean ) * ( v - mean );
		var /= std::max< size_t >( 1, c.perRow.size() - 1 );
		checkNamed( "clicks rate", std::abs( got - expected ) <= tol,
		            fmt( "CNR %4.1f dB: %6ld clicks in %.3f s (%d fields) = %9.1f /s; the sampled discriminator's "
		                 "exact rate %9.1f /s; |%.0f| <= %.0f (4 sigma Poisson + %.1f of integration)",
		                 cnr, got, c.seconds, fields, got / c.seconds, exact.total(), got - expected, tol, numeric ) );
		note( "Rice, continuous time, r erfc( sqrt rho ) = %.1f /s (r = %.3f MHz, rho = %.3f): the %dx discriminator "
		      "slips %.2f%% less often",
		      rice.total(), m.rmsHz() / 1e6, rho, link::kOversample, 100.0 * ( 1.0 - exact.total() / rice.total() ) );
		note( "+%ld / -%ld; per-row index of dispersion %.3f (Poisson: 1)", c.plus, c.minus,
		      mean > 0 ? var / mean : 0.0 );
	}

	if( hooks.linearise || hooks.flip )
		return g_failures - before;

	//Convergence: the same channel at four times the discriminator's rate.
	{
		p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( 4.0f ) );
		Count lo, hi;
		for( int f = 0; f < 2; ++f )
		{
			p.SetOversampleForTest( link::kOversample );
			rig.field( 5000 + f );
			probeFrame( p, 0, lo );
			p.SetOversampleForTest( 4 * link::kOversample );
			rig.field( 5000 + f );
			probeFrame( p, 0, hi );
		}
		p.SetOversampleForTest( link::kOversample );
		const auto& r    = p.ResolvedForTest();
		const double rice = riceRates( noiseMoments( r.noiseTaps, r.noiseSigma, r.fineRate, 0.0 ) ).total();
		const double a = ( lo.plus + lo.minus ) / lo.seconds, b = ( hi.plus + hi.minus ) / hi.seconds;
		const double expected = rice * hi.seconds;
		const double got      = static_cast< double >( hi.plus + hi.minus );
		checkNamed( "clicks continuous", std::abs( got - expected ) <= 4.0 * std::sqrt( expected ),
		            fmt( "4 dB, the same discriminator run at %dx: %.0f /s against Rice's %.0f /s in continuous time; "
		                 "|%.0f| <= %.0f clicks (4 sigma Poisson). At the shipped %dx: %.0f /s.",
		                 4 * link::kOversample, b, rice, got - expected, 4.0 * std::sqrt( expected ), link::kOversample, a ) );
	}
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --polarity
//
// The carrier held off the IF's centre by white: the uplink sends 0.7 V on
// every sample, which the pre-emphasis passes at its DC gain, so the carrier
// sits df = Deviation x g x ( 0.7 - 0.2 ) above centre. Rice's modulation
// term: the extra clicks are all negative, pulling the output toward the
// centre -- black sparklies on white. Each sign's count against the exact
// rates for the shifted spectrum, and the ratio.
//---------------------------------------------------------------------------
int runPolarity( int width, int height, const Hooks& hooks = {} )
{
	heading( "--polarity: near white, more negative clicks, by Rice's modulation term" );
	SignalRig rig;
	if( !rig.begin( width, height, hooks ) )
		return 1;
	Downlink& p = rig.session.plugin;
	p.SetTestSignalForTest( 1, static_cast< float >( link::kWhiteVolts ) );
	p.SetFloatParameter( Downlink::DL_DISPERSAL, 0.0f );

	const int before = g_failures;
	int64_t field    = 2000;
	for( double cnr : { 6.0, 8.0 } )
	{
		p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( cnr ) ) );
		rig.field( field );
		Count c;
		int fields = 0;
		const auto& r = p.ResolvedForTest();
		Rates rice;
		Moments m;
		double dphi0 = 0;
		while( true )
		{
			if( !probeFrame( p, 0, c ) )
				return 1;
			++fields;
			//The offset is read from the uplink's own record of what it sent.
			dphi0 = c.dphiSum / c.samples;
			m     = noiseMoments( r.noiseTaps, r.noiseSigma, r.fineRate, dphi0 );
			rice  = riceRates( m );
			if( rice.plus * c.seconds >= 1500.0 || fields >= 40 )
				break;
			rig.field( ++field );
		}
		++field;
		const double df     = dphi0 * r.fineRate / kTau;
		const Rates coarse  = discreteRates( r.noiseTaps, r.noiseSigma, r.fineRate, dphi0, 0.025 );
		const Rates exact   = discreteRates( r.noiseTaps, r.noiseSigma, r.fineRate, dphi0, 0.0125 );
		const double expP   = exact.plus * c.seconds, expM = exact.minus * c.seconds;
		const double numP   = std::abs( exact.plus - coarse.plus ) / 3.0 * c.seconds;
		const double numM   = std::abs( exact.minus - coarse.minus ) / 3.0 * c.seconds;
		const double tolP   = 4.0 * std::sqrt( expP ) + numP, tolM = 4.0 * std::sqrt( expM ) + numM;
		note( "CNR %.1f dB: carrier %.3f MHz above centre (Deviation %.2f MHz/V x %.4f x 0.5 V = %.3f)", cnr, df / 1e6,
		      r.deviation, link::PreDcGain(), r.deviation * link::PreDcGain() * 0.5 );
		checkNamed( "polarity positive", std::abs( c.plus - expP ) <= tolP,
		            fmt( "+ clicks %ld against %.0f; |%.0f| <= %.0f", c.plus, expP, c.plus - expP, tolP ) );
		checkNamed( "polarity negative", std::abs( c.minus - expM ) <= tolM,
		            fmt( "- clicks %ld against %.0f; |%.0f| <= %.0f", c.minus, expM, c.minus - expM, tolM ) );
		const double ratio   = double( c.plus ) / std::max< long >( 1, c.minus );
		const double predict = exact.plus / exact.minus;
		const double tolR    = predict * ( 4.0 * std::sqrt( 1.0 / expP + 1.0 / expM ) + numP / expP + numM / expM );
		checkNamed( "polarity ratio", std::abs( ratio - predict ) <= tolR,
		            fmt( "+/- = %.3f against %.3f; |%.3f| <= %.3f (delta method, 4 sigma)", ratio, predict,
		                 ratio - predict, tolR ) );
		const Rates book = riceTextbook( m.rmsHz(), 1.0 / m.b0, df );
		note( "Rice in continuous time for this spectrum: +/- = %.3f (%.0f / %.0f /s); the textbook's large-rho form "
		      "r erfc( sqrt rho ) / 2 against it + |df| exp( -rho ): %.3f",
		      rice.plus / rice.minus, rice.plus, rice.minus, book.plus / book.minus );
	}
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --threshold
//
// A flat grey, and the noise in the demodulated video (after the lowpass and
// the de-emphasis, before the clamp) against CNR. The prediction is built
// from the chain's own taps, in the time domain at the fine rate:
//
//   linear:  psi = Im( m ), the discriminator a difference, the link a sum
//            over Sub fine samples, the lowpass, every second sample, the
//            de-emphasis. Var = sum a( k ) a( k' ) R_y( k - k' ).
//   clicks:  Campbell: rate / fs x ( 2 pi )^2 x sum g( k )^2, each click a
//            whole cycle into the same chain, at Rice's rate.
//
// SNR is 0.7 V peak-to-peak luminance over the rms noise, unweighted. The
// classical offset -- 2 ( 0.7 D )^2 / integral |DE|^2 f^2 S( f ) over a brick
// wall at the video bandwidth, with F.405's analogue network and the Gaussian
// IF -- is reported beside it.
//---------------------------------------------------------------------------
struct ChainNoise
{
	double linear = 0;   //V^2
	double perClick = 0; //V^2 per ( click / fine sample )
};

ChainNoise chainNoise( const Downlink::Resolved& r, double dphi0 )
{
	const int sub   = r.sub;
	const double c1 = 1.0 / ( ( kTau * 1e6 / r.fineRate ) * sub * r.deviation );
	//g( k ): the weight of d[ k ] in the video sample j = 0.
	//link o = c1 sum over k in [ o sub - sub/2 + 1, o sub + sub/2 ]
	//q_j    = sum_i Lpf[ i ] link[ 2 j - i ],   v_j = sum_n De[ n ] q[ j - n ].
	const int nDe    = static_cast< int >( r.deTaps.size() );
	const long oLo   = -2L * ( nDe - 1 ) - link::kLpfHalf, oHi = link::kLpfHalf;
	const long kLo   = oLo * sub - sub / 2, kHi = oHi * sub + sub / 2 + 1;
	std::vector< double > g( static_cast< size_t >( kHi - kLo + 1 ), 0.0 );
	for( int n = 0; n < nDe; ++n )
		for( int i = -link::kLpfHalf; i <= link::kLpfHalf; ++i )
		{
			const long o   = -2L * n - i;
			const double w = r.deTaps[ static_cast< size_t >( n ) ] * r.lpfTaps[ static_cast< size_t >( i + link::kLpfHalf ) ] * c1;
			for( long k = o * sub - sub / 2 + 1; k <= o * sub + sub / 2; ++k )
				g[ static_cast< size_t >( k - kLo ) ] += w;
		}
	ChainNoise out;
	for( double v : g )
		out.perClick += v * v;

	//a( k ) = g( k ) - g( k + 1 ): the weight of psi[ k ] (d[ k ] = psi[ k ] - psi[ k-1 ]).
	std::vector< double > a( g.size() + 1, 0.0 );
	for( size_t k = 0; k < g.size(); ++k )
	{
		a[ k + 1 ] += g[ k ];
		a[ k ] -= g[ k ];
	}
	const int half = static_cast< int >( r.noiseTaps.size() / 2 );
	std::vector< double > Ry( static_cast< size_t >( 4 * half + 1 ), 0.0 );
	for( int lag = -2 * half; lag <= 2 * half; ++lag )
	{
		double s = 0.0;
		for( int i = -half; i <= half; ++i )
		{
			const int j = i + lag;
			if( j >= -half && j <= half )
				s += r.noiseTaps[ static_cast< size_t >( i + half ) ] * r.noiseTaps[ static_cast< size_t >( j + half ) ];
		}
		//Re R_m[ lag ] / 2, with the carrier-frame rotation cos( lag dphi0 ).
		Ry[ static_cast< size_t >( lag + 2 * half ) ] = 0.5 * r.noiseSigma * r.noiseSigma * s * std::cos( lag * dphi0 );
	}
	const long na = static_cast< long >( a.size() );
	for( long x = 0; x < na; ++x )
	{
		if( a[ static_cast< size_t >( x ) ] == 0.0 )
			continue;
		for( long lag = -2 * half; lag <= 2 * half; ++lag )
		{
			const long y = x + lag;
			if( y >= 0 && y < na )
				out.linear += a[ static_cast< size_t >( x ) ] * a[ static_cast< size_t >( y ) ] * Ry[ static_cast< size_t >( lag + 2 * half ) ];
		}
	}
	return out;
}

double classicalOffsetDb( double deviationMHz, double videoMHz, double ifMHz, bool de )
{
	const double sigma = link::IfSigmaHz( ifMHz * 1e6 );
	const double fv    = videoMHz * 1e6;
	const int n        = 20000;
	double integral    = 0.0;
	for( int i = 0; i <= n; ++i )
	{
		const double f  = -fv + 2.0 * fv * i / n;
		const double w  = ( i == 0 || i == n ) ? 1.0 : ( i % 2 ? 4.0 : 2.0 );
		const double de2 = de ? std::pow( 10.0, -link::F405Db( f / 1e6 ) / 10.0 ) : 1.0;
		const double S  = std::exp( -f * f / ( 2.0 * sigma * sigma ) ) / ( std::sqrt( kTau ) * sigma );
		integral += w * de2 * f * f * S;
	}
	integral *= ( 2.0 * fv / n ) / 3.0;
	const double D = deviationMHz * 1e6;
	return 10.0 * std::log10( 2.0 * ( 0.7 * D ) * ( 0.7 * D ) / integral );
}

/// Measured noise variance of the demodulated grey, V^2, over `fields`.
double measureVideoNoise( SignalRig& rig, int64_t firstField, int fields )
{
	double sum = 0.0;
	long n     = 0;
	for( int f = 0; f < fields; ++f )
	{
		rig.field( firstField + f );
		int w = 0, h = 0;
		const std::vector< float > v = rig.session.buffer( Downlink::Buffer::Video, w, h );
		for( int y = 0; y < h; ++y )
		{
			double mean = 0.0;
			int count   = 0;
			for( int x = 80; x < w - 16; ++x, ++count )
				mean += v[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			mean /= count;
			for( int x = 80; x < w - 16; ++x )
			{
				const double e = v[ ( static_cast< size_t >( y ) * w + x ) * 4 ] - mean;
				sum += e * e;
				++n;
			}
		}
	}
	return sum / n;
}

struct KneeModel
{
	std::function< double( double ) > linear;//V^2 at CNR dB
	std::function< double( double ) > clicks;
};

/// The CNR where total noise is 1 dB over the linear law, by bisection.
double predictedKnee( const KneeModel& m, double clickScale, double linearBias )
{
	auto excess = [ & ]( double cnr ) {
		const double lin = m.linear( cnr ) * ( 1.0 + linearBias / std::pow( 10.0, cnr / 10.0 ) );
		return ( lin + clickScale * m.clicks( cnr ) ) / m.linear( cnr ) - std::pow( 10.0, 0.1 );
	};
	double lo = -5.0, hi = 25.0;
	for( int i = 0; i < 80; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		( excess( mid ) > 0 ? lo : hi ) = mid;
	}
	return 0.5 * ( lo + hi );
}

int runThreshold( int width, int height, const Hooks& hooks = {}, bool quick = false )
{
	heading( "--threshold: SNR = CNR + the FM improvement above the knee; the knee where Rice puts it" );
	SignalRig rig;
	if( !rig.begin( width, height, hooks ) )
		return 1;
	Downlink& p = rig.session.plugin;
	const double grey = 0.35;
	p.SetTestSignalForTest( 1, static_cast< float >( grey ) );
	p.SetFloatParameter( Downlink::DL_DISPERSAL, 0.0f );
	const int before = g_failures;

	double knees[ 2 ] = { 0, 0 }, predicted[ 2 ] = { 0, 0 }, kneeTol[ 2 ] = { 0, 0 };
	for( int demod = 0; demod < ( quick ? 1 : 2 ); ++demod )
	{
		p.SetFloatParameter( Downlink::DL_DEMODULATOR, static_cast< float >( demod ) );
		//Everything but the noise's size is fixed by the controls; the noise
		//scales as sigma^2, so the chain is analysed once at sigma = 1.
		p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( 20.0f ) );
		rig.field( 1 );
		Downlink::Resolved r0 = p.ResolvedForTest();
		const double sigma20  = r0.noiseSigma;
		r0.noiseSigma         = 1.0;
		//The carrier offset for grey. The NOMINAL deviation: a receiver that
		//believes in another one is a broken receiver, and must be seen.
		const double dfHz   = r0.deviation * 1e6 * link::PreDcGain() * ( grey - link::kRestVolts );
		const bool rotate   = demod == controls::kDiscriminator;
		const double dphi0  = rotate ? kTau * dfHz / r0.fineRate : 0.0;
		const ChainNoise cn = chainNoise( r0, dphi0 );
		const Moments m1    = noiseMoments( r0.noiseTaps, 1.0, r0.fineRate, dphi0 );
		auto sigma2At       = [ & ]( double cnr ) {
			const double s = sigma20 * std::pow( 10.0, -( cnr - 20.0 ) / 20.0 );
			return s * s;
		};
		KneeModel model;
		model.linear = [ & ]( double cnr ) { return cn.linear * sigma2At( cnr ); };
		model.clicks = [ & ]( double cnr ) {
			Moments m = m1;
			const double s2 = sigma2At( cnr );
			m.b0 *= s2;
			m.b1 *= s2;
			m.b2 *= s2;
			return riceRates( m ).total() / r0.fineRate * kTau * kTau * cn.perClick;
		};

		//Above the knee: slope and offset.
		std::vector< std::pair< double, double > > points;
		for( double cnr : { 14.0, 17.0, 20.0, 23.0 } )
		{
			const double lin   = model.linear( cnr );
			const double clk   = model.clicks( cnr );
			p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( cnr ) ) );
			const double meas  = measureVideoNoise( rig, 100, 3 );
			const double snr   = 10.0 * std::log10( 0.49 / meas );
			const double pred  = 10.0 * std::log10( 0.49 / ( lin + clk ) );
			//Tolerance: 4 standard errors of a variance of ~1.3 M samples that
			//the lowpass correlates over ~5 (so 2.6e5 independent), plus the
			//linearisation's 1 / ( 2 CNR ), in dB.
			const double rel   = 4.0 * std::sqrt( 2.0 / 2.6e5 ) + 0.5 / std::pow( 10.0, cnr / 10.0 );
			const double tolDb = 10.0 * std::log10( 1.0 + rel );
			points.push_back( { cnr, snr } );
			checkNamed( "threshold offset", std::abs( snr - pred ) <= tolDb,
			            fmt( "%s CNR %4.1f dB: SNR %.3f dB, predicted %.3f (improvement %.3f dB); |%.3f| <= %.3f",
			                 controls::DemodulatorName( demod ), cnr, snr, pred, pred - cnr, snr - pred, tolDb ) );
		}
		{
			//Least-squares slope over the four points.
			double mx = 0, my = 0;
			for( auto& q : points )
				mx += q.first, my += q.second;
			mx /= points.size();
			my /= points.size();
			double sxy = 0, sxx = 0;
			for( auto& q : points )
				sxy += ( q.first - mx ) * ( q.second - my ), sxx += ( q.first - mx ) * ( q.first - mx );
			const double slope = sxy / sxx;
			//Each point within its tolerance (at worst the 14 dB one), through
			//the fit: sum |x - mean| / sxx times that.
			double sabs = 0;
			for( auto& q : points )
				sabs += std::abs( q.first - mx );
			const double tolS = 10.0 * std::log10( 1.0 + 4.0 * std::sqrt( 2.0 / 2.6e5 ) + 0.5 / std::pow( 10.0, 1.4 ) ) * sabs / sxx;
			checkNamed( "threshold slope", std::abs( slope - 1.0 ) <= tolS,
			            fmt( "%s: %.4f dB per dB from 14 to 23 dB; |%.4f| <= %.4f", controls::DemodulatorName( demod ), slope,
			                 slope - 1.0, tolS ) );
		}
		if( demod == controls::kDiscriminator )
		{
			const Downlink::Resolved& r = r0;
			note( "classical FM improvement (F.405 network, brick wall at %.1f MHz, Gaussian IF): %.3f dB; the chain's "
			      "own: %.3f dB",
			      r.videoMHz, classicalOffsetDb( r.deviation, r.videoMHz, r.ifMHz, r.de ),
			      10.0 * std::log10( 0.49 / model.linear( 20.0 ) ) - 20.0 );
		}

		if( quick )
			continue;

		//The knee: sweep down in half-dB steps until the noise is 1 dB over
		//the linear law, and interpolate.
		double prevCnr = 0, prevEx = 0, knee = NAN;
		for( double cnr = 16.0; cnr >= 2.0; cnr -= 0.5 )
		{
			p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( cnr ) ) );
			const double meas = measureVideoNoise( rig, 300 + static_cast< int64_t >( cnr * 10 ), 2 );
			const double ex   = 10.0 * std::log10( meas / model.linear( cnr ) );
			if( ex >= 1.0 && cnr < 16.0 )
			{
				knee = cnr + ( prevCnr - cnr ) * ( ex - 1.0 ) / ( ex - prevEx );
				break;
			}
			prevCnr = cnr;
			prevEx  = ex;
		}
		const double pk = predictedKnee( model, 1.0, 0.0 );
		//Tolerance: the model's own two approximations, each as the knee shift
		//it causes, plus a quarter of the grid. A click is not an impulse but a
		//swing lasting ~1 / r: through a 5 MHz lowpass that is worth at most a
		//factor 1.5 in its energy either way. The linear law misses
		//arg( 1 + m )'s second-order term, relative 1 / ( 2 CNR ).
		const double sClick = std::max( std::abs( predictedKnee( model, 1.5, 0.0 ) - pk ),
		                                std::abs( predictedKnee( model, 1.0 / 1.5, 0.0 ) - pk ) );
		const double sLin   = std::abs( predictedKnee( model, 1.0, 0.5 ) - pk );
		const double tol    = sClick + sLin + 0.125;
		knees[ demod ]      = knee;
		predicted[ demod ]  = pk;
		kneeTol[ demod ]    = tol;
		checkNamed( "threshold knee", std::isfinite( knee ) && std::abs( knee - pk ) <= tol,
		            fmt( "%s: noise 1 dB over the linear law at CNR %.2f dB, predicted %.2f; |%.2f| <= %.2f "
		                 "(click shape %.2f + linearisation %.2f + grid 0.125)",
		                 controls::DemodulatorName( demod ), knee, pk, knee - pk, tol, sClick, sLin ) );
		//Below the knee it collapses: from the knee to 3 dB under it the SNR
		//falls by more than the 3 dB the linear law would lose.
		if( std::isfinite( knee ) )
		{
			p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( knee ) ) );
			const double atKnee = measureVideoNoise( rig, 900, 2 );
			p.SetFloatParameter( Downlink::DL_CNR, controls::CnrParam( static_cast< float >( knee - 3.0 ) ) );
			const double under  = measureVideoNoise( rig, 950, 2 );
			const double fall   = 10.0 * std::log10( under / atKnee );
			checkNamed( "threshold collapse", fall > 3.0,
			            fmt( "%s: SNR falls %.2f dB from the knee to 3 dB under it (the linear law: 3)",
			                 controls::DemodulatorName( demod ), fall ) );
		}
	}
	if( !quick )
	{
		const double moved = knees[ 0 ] - knees[ 1 ], pm = predicted[ 0 ] - predicted[ 1 ];
		const double tol   = kneeTol[ 0 ] + kneeTol[ 1 ];
		checkNamed( "threshold extension", std::isfinite( moved ) && std::abs( moved - pm ) <= tol,
		            fmt( "Threshold Ext. moves the knee %.2f dB, predicted %.2f; |%.2f| <= %.2f (stated noise-"
		                 "bandwidth improvement %.1f dB)",
		                 moved, pm, moved - pm, tol, link::kThresholdExtensionDb ) );
	}
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --dispersal
//
// A flat grey clip, a noiseless link, 2 MHz of dispersal, the picture read
// back in float at the output raster. The triangle is written again here from
// its definition -- 25 Hz, locked to the frame so an even field ramps up from
// -pp/2 to +pp/2 and an odd one back down -- and every output pixel is
// predicted through the same bilinear read of the 921 x 576 picture the output
// pass makes. Row y is 22 lines plus y half-lines into its field; video sample
// n of the active line is ( 64 + n ) / 4fsc further on.
//
//   Off:  Y + tri( t ) x ( 1 / g ) / D / 0.7, g the pre-emphasis DC gain.
//   Good: the same less tri at the porch window's centre, on the same row.
//   Slow: the same less a first-order average of the porch with tau = 2 ms,
//         taken here in continuous time by quadrature.
//---------------------------------------------------------------------------
double triangleAt( int parity, double intoField, double pp )
{
	double cyc = parity + intoField / 0.02;
	cyc -= 2.0 * std::floor( cyc / 2.0 );
	return cyc < 1.0 ? pp * ( cyc - 0.5 ) : pp * ( 1.5 - cyc );
}

double rowStart( int y )
{
	return 22.0 * 64e-6 + y * 32e-6;
}

/// Float arithmetic's bound on the chain, in luma: nine passes, none summing
/// more than 128 terms of at most a volt, each term good to 2^-24.
constexpr double kFloatBound = 9.0 * 128.0 * 5.96e-8 / 0.7;

int runDispersal( int width, int height, const Hooks& hooks = {} )
{
	heading( "--dispersal: the 25 Hz triangle, and what each clamp leaves of it" );
	const int before = g_failures;
	const double pp  = 2.0;
	const double code = 128.0 / 255.0;

	for( int mode : { controls::kClampOff, controls::kClampSlow, controls::kClampGood } )
	{
		if( hooks.skipTau && mode != controls::kClampSlow )
			continue;
		Session s;
		s.floatOutput = true;
		applyHooks( s.plugin, hooks );
		s.plugin.SetNoiseScaleForTest( 0.0f );
		s.plugin.SetFloatParameter( Downlink::DL_DISPERSAL, controls::DispersalParam( static_cast< float >( pp ) ) );
		s.plugin.SetFloatParameter( Downlink::DL_CLAMP, static_cast< float >( mode ) );
		if( !s.begin( width, height ) )
			return 1;
		const Image clip = buildFlat( width, height, 128 );
		const auto& r    = s.plugin.ResolvedForTest();
		const double conv = std::pow( 10.0, link::kPreA / 20.0 ) / r.deviation / 0.7;//MHz -> luma
		const double fsV  = link::kFsVideo;
		const double tau  = link::kSlowClampSeconds;
		const double margin = r.preTaps.size() / 2.0 + r.deTaps.size() + link::kLpfHalf / 2.0 + 4.0;

		double worst = 0, worstTol = 0, lo = 1e9, hi = -1e9, residualMax = 0, goodBound = 0;
		double plo = 1e9, phi_ = -1e9;
		long pixels = 0, bad = 0;
		for( int64_t field : { int64_t( 4000 ), int64_t( 4001 ) } )
		{
			s.renderAt( fieldTime( field ), clip );
			const std::vector< float > out = s.readBackFloat();
			const int parity               = static_cast< int >( field & 1 );

			//The clamp level per row, predicted.
			std::vector< double > clampAt( link::kRows, 0.0 );
			for( int y = 0; y < link::kRows; ++y )
			{
				const double tPorch = rowStart( y ) + ( link::kClampFirst + 0.5 * ( link::kClampCount - 1 ) ) / fsV;
				if( mode == controls::kClampGood )
					clampAt[ y ] = triangleAt( parity, tPorch, pp );
				else if( mode == controls::kClampSlow )
				{
					//c( t ) = integral_0^inf ( 1 / tau ) exp( -u / tau ) P( t - u ) du,
					//P the porch's dispersal, by the midpoint rule at 1 us over 12 tau.
					double c = 0.0;
					const double du = 1e-6;
					for( double u = 0.5 * du; u < 12.0 * tau; u += du )
						c += std::exp( -u / tau ) * triangleAt( parity, tPorch - u, pp );
					clampAt[ y ] = c * du / tau;
				}
			}
			auto predict = [ & ]( int n, int y ) {
				const double t = rowStart( y ) + ( 64 + n ) / fsV;
				return code + ( triangleAt( parity, t, pp ) - clampAt[ y ] ) * conv;
			};
			for( int row = 0; row < height; ++row )
			{
				const double yf = std::clamp( ( row + 0.5 ) * link::kRows / height - 0.5, 0.0, link::kRows - 1.0 );
				const int y0    = std::min( static_cast< int >( yf ), link::kRows - 2 );
				const double ty = yf - y0;
				double rowSum = 0, predSum = 0;
				int rowCount  = 0;
				for( int c = 0; c < width; ++c )
				{
					//Clear of the active line's two ends by every filter's
					//support, as --identity: the porch is at blanking, and
					//the step onto the picture is not what this measures.
					const double nRaw = ( c + 0.5 ) * link::kActive / width - 0.5;
					if( nRaw < margin || nRaw > link::kActive - 1 - margin )
						continue;
					const double nf = std::clamp( nRaw, 0.0, link::kActive - 1.0 );
					const int n0    = std::min( static_cast< int >( nf ), link::kActive - 2 );
					const double tn = nf - n0;
					const double pred = ( 1 - ty ) * ( ( 1 - tn ) * predict( n0, y0 ) + tn * predict( n0 + 1, y0 ) )
					                    + ty * ( ( 1 - tn ) * predict( n0, y0 + 1 ) + tn * predict( n0 + 1, y0 + 1 ) );
					const float* px   = &out[ ( static_cast< size_t >( row ) * width + c ) * 4 ];
					const double meas = 0.299 * px[ 0 ] + 0.587 * px[ 1 ] + 0.114 * px[ 2 ];
					//Tolerance: float, the de-emphasis's group delay (tau_z - tau_p,
					//0.41 us) on the along-row slope, and for Slow the recursion's
					//row step against continuous time (half a row of porch slope,
					//taken as a whole row) plus its 576-row truncation.
					const double slopeMHzPerS = 2.0 * pp * 25.0;
					double tol = kFloatBound + slopeMHzPerS * 0.41e-6 * conv;
					if( mode == controls::kClampSlow )
						tol += slopeMHzPerS * link::kRowSeconds * conv
						       + std::pow( 1.0 - r.slowAlpha, link::kHistoryRows ) * 0.5 * pp * conv;
					const double e = std::abs( meas - pred );
					if( e > worst )
						worst = e, worstTol = tol;
					if( e > tol )
						++bad;
					++pixels;
					rowSum += meas;
					predSum += pred;
					++rowCount;
					residualMax = std::max( residualMax, std::abs( meas - code ) );
					//Good's per-line bound: the triangle moves at most
					//slope x ( the farthest active sample from the porch centre ).
					goodBound = slopeMHzPerS * ( ( 64 + link::kActive - 1 ) - ( link::kClampFirst + 0.5 * ( link::kClampCount - 1 ) ) ) / fsV * conv + tol;
				}
				rowSum /= std::max( rowCount, 1 );
				predSum /= std::max( rowCount, 1 );
				lo  = std::min( lo, rowSum );
				hi  = std::max( hi, rowSum );
				plo = std::min( plo, predSum );
				phi_ = std::max( phi_, predSum );
			}
		}
		const char* name = controls::ClampName( mode );
		const std::string label = std::string( "dispersal " ) + ( mode == 0 ? "off" : mode == 1 ? "slow" : "good" );
		checkNamed( label, bad == 0,
		            fmt( "Clamp %s: %ld of %ld pixels off the prediction; worst %.2e against %.2e", name, bad, pixels, worst,
		                 worstTol ) );
		if( mode == controls::kClampOff )
			note( "row means over an even and an odd field swing %.4f..%.4f, %.4f p-p of luma (predicted %.4f..%.4f): "
			      "the picture rocks at the field rate",
			      lo, hi, hi - lo, plo, phi_ );
		else if( mode == controls::kClampGood )
			checkNamed( "dispersal good bound", residualMax <= goodBound,
			            fmt( "Clamp Good: largest residual %.2e luma <= the per-line bound %.2e", residualMax, goodBound ) );
		else
			note( "Clamp Slow: largest residual %.4f luma (a first-order lag, slope x tau = %.4f)", residualMax,
			      2.0 * pp * 25.0 * tau * conv );
		s.end();
	}
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --identity
//
// Component, a noiseless link, no dispersal, Clamp Good, both emphasis
// networks, the widest video filter: four flat quadrants must come back as
// they went in, to within one 8-bit code, wherever no filter reaches across
// an edge. The margin is the sum of every support: the pre-emphasis (causal,
// at the link rate), the de-emphasis (causal), the lowpass, the discriminator
// and its interpolation, the resample and the output's bilinear read. Inside
// it, every filter sees a constant: the lowpass's taps sum to 1, the two
// networks' DC gains are g and 1 / g exactly, a noiseless discriminator
// returns the phase increment it was sent, and the clamp takes off a
// blanking porch. What is left is float (1e-4) and the 8-bit rounding (half a
// code), so a pixel can land one code away and no further.
//---------------------------------------------------------------------------
int runIdentity( int width, int height, const Hooks& hooks = {} )
{
	heading( "--identity: a clean link returns the input" );
	const int before = g_failures;
	Session s;
	applyHooks( s.plugin, hooks );
	s.plugin.SetNoiseScaleForTest( 0.0f );
	s.plugin.SetFloatParameter( Downlink::DL_DISPERSAL, 0.0f );
	s.plugin.SetFloatParameter( Downlink::DL_COMPOSITE, static_cast< float >( controls::kComponent ) );
	s.plugin.SetFloatParameter( Downlink::DL_CLAMP, static_cast< float >( controls::kClampGood ) );
	s.plugin.SetFloatParameter( Downlink::DL_VIDEO_BANDWIDTH, 1.0f );
	if( !s.begin( width, height ) )
		return 1;
	const Image card = buildQuadrants( width, height );
	s.render( 3, card );
	const Image out = s.readBack();
	const auto& r   = s.plugin.ResolvedForTest();

	const double margin = r.preTaps.size() / 2.0 + r.deTaps.size() + link::kLpfHalf / 2.0 + 4.0;//video samples
	long probes = 0, bad = 0;
	int worst   = 0;
	for( int y = 0; y < height; ++y )
	{
		const double yf = ( y + 0.5 ) * link::kRows / height - 0.5;
		if( std::abs( yf - ( link::kRows / 2.0 - 0.5 ) ) < 2.0 )
			continue;
		for( int x = 0; x < width; ++x )
		{
			const double nf = ( x + 0.5 ) * link::kActive / width - 0.5;
			const double edges[] = { -0.5, link::kActive / 2.0 - 0.5, link::kActive - 0.5 };
			bool near = false;
			for( double e : edges )
				near = near || std::abs( nf - e ) < margin;
			if( near )
				continue;
			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			for( int c = 0; c < 3; ++c )
			{
				const int d = std::abs( int( out[ i + c ] ) - int( card[ i + c ] ) );
				worst       = std::max( worst, d );
				if( d > 1 )
					++bad;
			}
			++probes;
		}
	}
	checkNamed( "identity", probes > 0 && bad == 0,
	            fmt( "%ld interior pixels (margin %.0f video samples = %.1f px); %ld channels more than one code off; "
	                 "worst %d",
	                 probes, margin, margin * width / link::kActive, bad, worst ) );
	checkNamed( "identity coverage", probes >= static_cast< long >( width ) * height / 20,
	            fmt( "the interior is %.1f%% of the frame (at least 5%%, or the check has lost its subject)",
	                 100.0 * probes / ( double( width ) * height ) ) );
	s.end();
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --seed and --resize
//---------------------------------------------------------------------------
std::vector< float > videoFor( int width, int height, int seed, int64_t field, Image* picture = nullptr )
{
	Session s;
	s.plugin.SetFloatParameter( Downlink::DL_SEED, static_cast< float >( seed ) );
	if( !s.begin( width, height ) )
		return {};
	const Image clip = buildFlat( width, height, 128 );
	s.renderAt( fieldTime( field ), clip );
	int w, h;
	std::vector< float > v = s.buffer( Downlink::Buffer::Video, w, h );
	if( picture )
		*picture = s.readBack();
	s.end();
	return v;
}

int runSeed( int width, int height, const Hooks& = {} )
{
	heading( "--seed: the noise is a function of ( seed, field ), and of nothing else" );
	const int before = g_failures;
	Image a, b;
	const std::vector< float > v1 = videoFor( width, height, 7, 123, &a );
	const std::vector< float > v2 = videoFor( width, height, 7, 123, &b );
	const std::vector< float > v3 = videoFor( width, height, 8, 123 );
	const std::vector< float > v4 = videoFor( width, height, 7, 124 );
	const int ow = width == 320 ? 1280 : 320, oh = width == 320 ? 720 : 180;
	const std::vector< float > v5 = videoFor( ow, oh, 7, 123 );
	checkNamed( "seed repeat", !v1.empty() && v1 == v2 && a == b,
	            "the same seed and field twice: the demodulated video and the picture bit-identical" );
	checkNamed( "seed differs", v1 != v3, "another seed: different noise" );
	checkNamed( "seed field", v1 != v4, "the next field: different noise" );
	checkNamed( "seed raster", v1 == v5,
	            fmt( "at %dx%d and %dx%d the demodulated video is bit-identical (the link runs at its own raster)", width,
	                 height, ow, oh ) );
	return g_failures - before;
}

int runResize( int width, int height, const Hooks& = {} )
{
	heading( "--resize: a resize mid-run leaves nothing behind" );
	const int before = g_failures;
	Session s;
	if( !s.begin( width, height ) )
		return 1;
	const Image card = buildCard( width, height );
	s.render( 0, card );
	s.render( 1, card );
	s.resize( width * 2, height * 2 );
	s.render( 2, buildCard( width * 2, height * 2 ) );
	s.resize( width, height );
	s.render( 3, card );
	const Image after = s.readBack();
	s.end();

	Session fresh;
	fresh.begin( width, height );
	fresh.render( 3, card );
	const Image clean = fresh.readBack();
	fresh.end();
	checkNamed( "resize", after == clean,
	            fmt( "%dx%d -> %dx%d -> %dx%d: the frame after equals a fresh instance's, byte for byte (the plugin keeps "
	                 "no picture between frames; the Slow clamp's history is computed, not remembered)",
	                 width, height, width * 2, height * 2, width, height ) );
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --emphasis
//
// Offline: each network's response, from its truncated taps, against F.405's
// own curve, within F.405's own tolerance +/-( 0.1 + 0.05 f / fc ) dB, from
// 0.01 MHz to fc = 5 MHz.
//
// In GL: one sine per row, 0.05 to 5.05 MHz, through the noiseless link with
// both networks and with neither; the amplitude ratio at each row's frequency,
// fitted by least squares from the demodulated video, is flat within the sum
// of the two networks' measured departures from F.405 (which cancel exactly
// only if both are exact) and 0.001 dB of fitting.
//---------------------------------------------------------------------------
double responseDb( const std::vector< double >& taps, double fMHz, double fs )
{
	double re = 0, im = 0;
	for( size_t n = 0; n < taps.size(); ++n )
	{
		const double a = -kTau * fMHz * 1e6 * n / fs;
		re += taps[ n ] * std::cos( a );
		im += taps[ n ] * std::sin( a );
	}
	return 10.0 * std::log10( re * re + im * im );
}

double g_emphasisBound = 0.0;

int runEmphasisOffline( const Hooks& hooks = {} )
{
	heading( "--emphasis (offline): both networks meet ITU-R F.405-1 within its own tolerance" );
	const int before = g_failures;
	std::vector< double > pre = link::PreEmphasisTaps();
	if( hooks.pe405Detune != 0.0 )
	{
		link::FirstOrder n = link::PreEmphasisNetwork();
		n.pole *= 1.0 + hooks.pe405Detune;
		pre.assign( 1, n.gain );
		double tail = n.gain * ( n.pole - n.zero );
		for( int i = 1; i < link::kMaxPreTaps; ++i, tail *= n.pole )
			pre.push_back( tail );
	}
	const std::vector< double > de = link::DeEmphasisTaps();
	double worstPre = 0, worstDe = 0, marginPre = 1e9, marginDe = 1e9;
	for( int i = 0; i <= 500; ++i )
	{
		const double f   = 0.01 + ( 5.0 - 0.01 ) * i / 500.0;
		const double tol = link::F405ToleranceDb( f, 5.0 );
		const double ep  = responseDb( pre, f, link::kFsLink ) - link::F405Db( f );
		const double ed  = responseDb( de, f, link::kFsVideo ) + link::F405Db( f );
		worstPre         = std::max( worstPre, std::abs( ep ) );
		worstDe          = std::max( worstDe, std::abs( ed ) );
		marginPre        = std::min( marginPre, tol - std::abs( ep ) );
		marginDe         = std::min( marginDe, tol - std::abs( ed ) );
	}
	checkNamed( "emphasis F.405 pre", marginPre >= 0.0,
	            fmt( "pre-emphasis (%zu taps at %.2f MHz): worst departure %.4f dB, margin to F.405's tolerance %.4f dB",
	                 pre.size(), link::kFsLink / 1e6, worstPre, marginPre ) );
	checkNamed( "emphasis F.405 de", marginDe >= 0.0,
	            fmt( "de-emphasis (%zu taps at %.2f MHz): worst departure %.4f dB, margin %.4f dB", de.size(),
	                 link::kFsVideo / 1e6, worstDe, marginDe ) );
	g_emphasisBound = worstPre + worstDe + 0.001;
	note( "the crossover: %.4f dB at 1.512 MHz (F.405: 0 dB)", link::F405Db( 1.512 ) );
	return g_failures - before;
}

std::vector< double > sineAmplitudes( Session& s, int64_t field, const Image& clip, double f0, double step )
{
	s.renderAt( fieldTime( field ), clip );
	int w, h;
	const std::vector< float > v = s.buffer( Downlink::Buffer::Video, w, h );
	std::vector< double > amps( static_cast< size_t >( h ) );
	for( int y = 0; y < h; ++y )
	{
		const double cyc = ( f0 + step * y ) * 1e6 / link::kFsVideo;
		//Least squares for DC + a sin + b cos over x in [150, 850].
		double A[ 3 ][ 3 ] = {}, B[ 3 ] = {};
		for( int x = 150; x <= 850; ++x )
		{
			const double basis[ 3 ] = { 1.0, std::sin( kTau * cyc * x ), std::cos( kTau * cyc * x ) };
			const double val        = v[ ( static_cast< size_t >( y ) * w + x ) * 4 ];
			for( int i = 0; i < 3; ++i )
			{
				B[ i ] += basis[ i ] * val;
				for( int j = 0; j < 3; ++j )
					A[ i ][ j ] += basis[ i ] * basis[ j ];
			}
		}
		//Gaussian elimination, 3 x 3.
		for( int c = 0; c < 3; ++c )
			for( int r = c + 1; r < 3; ++r )
			{
				const double f = A[ r ][ c ] / A[ c ][ c ];
				for( int k = c; k < 3; ++k )
					A[ r ][ k ] -= f * A[ c ][ k ];
				B[ r ] -= f * B[ c ];
			}
		double sol[ 3 ];
		for( int r = 2; r >= 0; --r )
		{
			double acc = B[ r ];
			for( int k = r + 1; k < 3; ++k )
				acc -= A[ r ][ k ] * sol[ k ];
			sol[ r ] = acc / A[ r ][ r ];
		}
		amps[ static_cast< size_t >( y ) ] = std::hypot( sol[ 1 ], sol[ 2 ] );
	}
	return amps;
}

int runEmphasis( int width, int height, const Hooks& hooks = {} )
{
	const int before = g_failures;
	runEmphasisOffline();
	heading( "--emphasis: pre-emphasis then de-emphasis, noiseless, is flat across the video band" );
	const double f0 = 0.05, step = 5.0 / ( link::kRows - 1 );
	std::vector< double > amps[ 2 ];
	for( int on = 0; on < 2; ++on )
	{
		Session s;
		applyHooks( s.plugin, hooks );
		s.plugin.SetNoiseScaleForTest( 0.0f );
		s.plugin.SetTestSignalForTest( 2, static_cast< float >( link::kRestVolts ), 0.1f, static_cast< float >( f0 ),
		                               static_cast< float >( step ) );
		s.plugin.SetFloatParameter( Downlink::DL_DISPERSAL, 0.0f );
		s.plugin.SetFloatParameter( Downlink::DL_VIDEO_BANDWIDTH, 1.0f );
		s.plugin.SetFloatParameter( Downlink::DL_PRE_EMPHASIS, static_cast< float >( on ) );
		s.plugin.SetFloatParameter( Downlink::DL_DE_EMPHASIS, static_cast< float >( on ) );
		if( !s.begin( width, height ) )
			return 1;
		amps[ on ] = sineAmplitudes( s, 10, buildFlat( width, height, 128 ), f0, step );
		s.end();
	}
	double worst = 0, worstF = 0;
	for( int y = 0; y < link::kRows; ++y )
	{
		const double db = 20.0 * std::log10( amps[ 1 ][ static_cast< size_t >( y ) ] / amps[ 0 ][ static_cast< size_t >( y ) ] );
		if( std::abs( db ) > worst )
			worst = std::abs( db ), worstF = f0 + step * y;
	}
	checkNamed( "emphasis flat", worst <= g_emphasisBound,
	            fmt( "576 tones 0.05..5.05 MHz: worst |pre x de| = %.4f dB at %.2f MHz; bound %.4f dB (the two "
	                 "networks' departures from F.405 + 0.001 dB of fitting)",
	                 worst, worstF, g_emphasisBound ) );
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --clock: a six-day host clock makes the same rows as a fresh one.
//---------------------------------------------------------------------------
int runClock( const Hooks& hooks = {} )
{
	heading( "--clock: Resolume's clock overflows a float; the rows must not notice" );
	const int before = g_failures;
	const double t1  = 7.049;//float( t2 ) rounds 13.5 ms up, across a field boundary
	const double t2  = t1 + 6.0 * 86400.0;//a whole number of 40 ms cycles
	link::RowTable a, b;
	link::MakeRowTable( t1, 2.0, a );
	link::MakeRowTable( hooks.floatClock ? static_cast< double >( static_cast< float >( t2 ) ) : t2, 2.0, b );
	double worst = 0;
	for( size_t i = 0; i < a.base.size(); ++i )
		worst = std::max( worst, static_cast< double >( std::abs( a.base[ i ] - b.base[ i ] ) ) );
	//Two float ULPs of the largest value (1 MHz): the table's floats may round
	//apart where a double product lands on a rounding boundary, no further.
	const double tol = 2.0 * std::ldexp( 1.0, -23 );
	checkNamed( "clock", a.parity == b.parity && worst <= tol,
	            fmt( "t = %.4f s and %.4f s: parity %d/%d, largest row difference %.2e MHz <= %.2e", t1, t2, a.parity,
	                 b.parity, worst, tol ) );
	note( "field %lld at six days: the index is in double and only its parity reaches the shader",
	      static_cast< long long >( b.field ) );
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	heading( "--names: nothing the host will silently truncate" );
	const int before = g_failures;
	Downlink plugin;
	const auto list  = listParameters( plugin );
	size_t longest   = 0;
	std::string longName;
	bool unique = true;
	for( size_t i = 0; i < list.size(); ++i )
	{
		if( list[ i ].name.size() > longest )
			longest = list[ i ].name.size(), longName = list[ i ].name;
		for( size_t j = i + 1; j < list.size(); ++j )
			unique = unique && list[ i ].name != list[ j ].name;
	}
	checkNamed( "names length", longest <= 16, fmt( "longest parameter name '%s', %zu characters", longName.c_str(), longest ) );
	checkNamed( "names unique", unique, "every parameter name is unique (--set and the sweep find them by name)" );
	checkNamed( "names plugin", std::strlen( "SW Downlink" ) <= 16, "'SW Downlink', 11 characters" );
	return g_failures - before;
}

//---------------------------------------------------------------------------
// --rice (offline): the harness's integrator against Rice's closed form.
//---------------------------------------------------------------------------
int runRice()
{
	heading( "--rice (offline): the exact-rate integrator reduces to r erfc( sqrt rho ) on an unmodulated carrier" );
	const int before = g_failures;
	Downlink plugin;
	const auto& r = plugin.ResolvedForTest();
	for( double cnr : { 4.0, 8.0, 12.0 } )
	{
		const double sigma = std::pow( 10.0, -cnr / 20.0 );
		const Moments m    = noiseMoments( r.noiseTaps, sigma, r.fineRate, 0.0 );
		const Rates got    = riceRates( m );
		const double want  = m.rmsHz() * std::erfc( std::sqrt( 1.0 / m.b0 ) );
		checkNamed( "rice integrator", std::abs( got.total() / want - 1.0 ) <= 1e-6 && std::abs( m.b0 / ( sigma * sigma ) - 1.0 ) <= 1e-9,
		            fmt( "CNR %.0f dB: %.6e /s against %.6e (relative %.1e); b0 / sigma^2 - 1 = %.1e; r = %.4f MHz "
		                 "(the control's sigma %.4f)",
		                 cnr, got.total(), want, got.total() / want - 1.0, m.b0 / ( sigma * sigma ) - 1.0, m.rmsHz() / 1e6,
		                 link::IfSigmaHz( r.ifMHz * 1e6 ) / 1e6 ) );
	}
	//The sampled discriminator's exact rate against Rice's as the sampling
	//rises: it must approach from below (a sampled crossing detector misses
	//crossings, it never invents them) and close in. 64x is left out: its
	//lag-one correlation is 0.998 and the integral's grid does not resolve
	//the conditional density (it moves 2% between grids); up to 32x it moves
	//less than a sixth of the gap it measures.
	for( double cnr : { 4.0, 10.0 } )
	{
		const double sigma = std::pow( 10.0, -cnr / 20.0 );
		double prevGap     = 1e9;
		bool below = true, closing = true;
		std::string line;
		double rice = 0;
		for( int k : { 8, 16, 32 } )
		{
			const double fs    = k * link::kFsVideo;
			const auto taps    = link::NoiseTaps( link::IfSigmaHz( r.ifMHz * 1e6 ), fs );
			const Moments m    = noiseMoments( taps, sigma, fs, 0.0 );
			rice               = riceRates( m ).total();
			const double exact = discreteRates( taps, sigma, fs, 0.0, 0.0125 ).total();
			const double gap   = 1.0 - exact / rice;
			below              = below && gap > 0.0;
			closing            = closing && gap < prevGap;
			prevGap            = gap;
			line += fmt( "  %dx %.2f%%", k, 100.0 * gap );
		}
		checkNamed( "rice sampled", below && closing,
		            fmt( "CNR %.0f dB: the sampled discriminator's shortfall from Rice (%.1f /s) by sampling rate:%s", cnr,
		                 rice, line.c_str() ) );
	}
	return g_failures - before;
}

int runThresholdQuick( int width, int height, const Hooks& hooks )
{
	return runThreshold( width, height, hooks, true );
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
struct NegativeCase
{
	const char* what;
	const char* mustFail;
	std::function< int() > run;
};

int runNegativeCases( const std::vector< NegativeCase >& cases )
{
	heading( "--negative: every check can FAIL, on the bound that should catch it" );
	int caught = 0;
	for( const NegativeCase& c : cases )
	{
		const int checks = g_checks, failures = g_failures;
		const bool quiet = g_quiet;
		g_failed.clear();
		g_quiet = true;
		std::printf( "   ...  %s\n", c.what );
		std::fflush( stdout );
		c.run();
		g_quiet            = quiet;
		const int ran      = g_checks - checks;
		const int failed   = g_failures - failures;
		const bool named   = std::any_of( g_failed.begin(), g_failed.end(),
		                                  [ & ]( const std::string& l ) { return l.rfind( c.mustFail, 0 ) == 0; } );
		//The negative run's own failures are the point; take them back out.
		g_checks   = checks;
		g_failures = failures;
		check( failed > 0 && named, "%s: %d of %d checks failed, '%s' among them", c.what, failed, ran, c.mustFail );
		caught += failed > 0 && named;
	}
	return caught;
}

std::vector< NegativeCase > offlineNegatives()
{
	return {
		{ "the host clock taken in float", "clock", [] { Hooks h; h.floatClock = true; return runClock( h ); } },
		{ "a pre-emphasis pole 5% off F.405", "emphasis F.405 pre",
		  [] { Hooks h; h.pe405Detune = 0.05; return runEmphasisOffline( h ); } },
	};
}

std::vector< NegativeCase > glNegatives( int width, int height )
{
	return {
		{ "the discriminator linearised (psi = Im m: no clicks)", "clicks rate",
		  [ = ] { Hooks h; h.linearise = true; return runClicks( width, height, h ); } },
		{ "the noise rotated the wrong way into the carrier's frame", "polarity",
		  [ = ] { Hooks h; h.flip = true; return runPolarity( width, height, h ); } },
		{ "the Slow clamp's time constant skipped", "dispersal slow",
		  [ = ] { Hooks h; h.skipTau = true; return runDispersal( width, height, h ); } },
		{ "the de-emphasis pole 5% off", "emphasis flat",
		  [ = ] { Hooks h; h.deDetune = 0.05; return runEmphasis( width, height, h ); } },
		{ "a receiver that believes the deviation is 2% higher", "identity",
		  [ = ] { Hooks h; h.rxDetune = 0.02; return runIdentity( width, height, h ); } },
		{ "a receiver that believes the deviation is 10% higher", "threshold offset",
		  [ = ] { Hooks h; h.rxDetune = 0.10; return runThresholdQuick( width, height, h ); } },
	};
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( const std::vector< std::string >& settings, int width, int height, int frames )
{
	Session session;
	for( const std::string& s : settings )
	{
		std::string error;
		applySetting( session.plugin, s, error );
	}
	if( !session.begin( width, height ) )
		return -1.0;
	const Image card = buildCard( width, height );
	for( int f = 0; f < 10; ++f )
		session.render( f, card );
	glFinish();
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int f = 0; f < frames; ++f )
			session.render( 10 + run * frames + f, card );
		glFinish();
		const double s = std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count();
		best           = std::min( best, s * 1000.0 / frames );
	}
	session.end();
	return best;
}

int runBench( const std::vector< std::string >& settings, int frames )
{
	struct Size
	{
		const char* name;
		int w, h;
	};
	const Size sizes[] = { { "1280x720 ", 1280, 720 }, { "1920x1080", 1920, 1080 }, { "3840x2160", 3840, 2160 } };
	std::printf( "%d frames each, best of three runs, after a 10-frame warm-up, glFinish both sides.\n", frames );
	std::printf( "Discriminator at %d x 4fsc = %.2f MHz; link raster %d x %d, video raster %d x %d.\n",
	             link::kOversample, link::kFsFine / 1e6, link::kLinkLine, link::kRows, link::kLine, link::kRows );
	std::printf( "resolution   PAL ms/frame   Component ms/frame   %% of a 60fps frame (PAL)\n" );
	for( const Size& s : sizes )
	{
		std::vector< std::string > pal = settings, comp = settings;
		comp.push_back( "Composite=1" );
		const double a = benchAt( pal, s.w, s.h, frames );
		const double b = benchAt( comp, s.w, s.h, frames );
		std::printf( "%s     %7.3f          %7.3f              %5.1f%%\n", s.name, a, b, a / 16.667 * 100.0 );
	}
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int number = 0;
	while( std::getline( file, line ) )
	{
		++number;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( number ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

int runPipe( Session& session, const std::string& scriptPath, float audioLevel )
{
	std::map< unsigned int, Track > automation;
	Track audioTrack;
	if( !scriptPath.empty() )
	{
		std::string error;
		const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
		if( !error.empty() )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
		for( const auto& entry : tracks )
		{
			//`@audio` is the harness's synthetic spectrum level, not a
			//parameter: it is what an operator's music would be.
			if( entry.first == "@audio" )
			{
				audioTrack = entry.second;
				continue;
			}
			const int index = indexOfParameter( session.plugin, entry.first );
			if( index < 0 )
			{
				std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
				return 2;
			}
			automation[ static_cast< unsigned int >( index ) ] = entry.second;
		}
	}

	Image frame( static_cast< size_t >( session.width ) * session.height * 4 );
	for( int index = 0;; ++index )
	{
		size_t got = 0;
		while( got < frame.size() )
		{
			const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
			if( n <= 0 )
				break;
			got += static_cast< size_t >( n );
		}
		//A partial frame at the end of a pipe is the end of the stream.
		if( got < frame.size() )
			break;

		for( const auto& track : automation )
			session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );
		const float level = !audioTrack.empty() ? valueAt( audioTrack, index ) : audioLevel;
		if( level >= 0.0f )
			injectSpectrum( session.plugin, level, index / 60.0 );

		if( !session.render( index, frame ) )
			return 1;

		const Image out = session.readBack();
		size_t written  = 0;
		while( written < out.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
			if( put <= 0 )
				break;
			written += static_cast< size_t >( put );
		}
		if( written < out.size() )
		{
			std::fprintf( stderr, "stdout closed at frame %d\n", index );
			return 1;
		}
	}
	return 0;
}

int dumpShaders( const std::string& dir )
{
	for( int i = 0; i < shaders::kAllCount; ++i )
	{
		const shaders::Named& s = shaders::kAll[ i ];
		const std::string ext   = std::strstr( s.text, "gl_Position" ) ? ".vert" : ".frag";
		std::ofstream out( dir + "/" + s.name + ext );
		out << s.text;
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s\n", ( dir + "/" + s.name + ext ).c_str() );
			return 1;
		}
	}
	std::printf( "%d\n", shaders::kAllCount );
	return 0;
}

void usage()
{
	std::printf(
		"dltest -- render and measure the Downlink satellite-link effect\n"
		"\n"
		"  --out PATH        render the test card through the plugin (default /tmp/downlink.png)\n"
		"  --size WxH        raster (default 1280x720)\n"
		"  --frames N        frames to render before reading back (default 30)\n"
		"  --source S        card (default), flat, quadrants\n"
		"  --level N         the flat source's code value (default 128)\n"
		"  --audio L         feed a synthetic spectrum at level L (0..1)\n"
		"  --set \"Name=V\"    set a parameter by display name (0..1 sliders; option index)\n"
		"  --list            every parameter, kind, default and range\n"
		"  --names --clock --emphasis --clicks --polarity --threshold --dispersal\n"
		"  --identity --seed --resize --negative\n"
		"                    the checks; see the header of this file\n"
		"  --offline         every check that needs no GL context\n"
		"  --allow-no-gl     with GL checks: SKIP loudly, not FAIL, when there is no context\n"
		"  --quiet           print failures and the summary only\n"
		"  --bench           ms/frame at 720p, 1080p, 4K (--bench-frames N)\n"
		"  --pipe            raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH     cues for --pipe: 'frame Name Value' ('@audio' is the spectrum level)\n"
		"  --dump-shaders D  write every shader the plugin compiles into D\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/downlink.png", scriptPath, sourceName = "card", dumpDir;
	int width = 1280, height = 720, frames = 30, level = 128, benchFrames = 30;
	float audioLevel = -1.0f;
	bool wantList = false, wantBench = false, wantPipe = false, allowNoGL = false;
	std::vector< std::string > settings, modes;

	for( int i = 1; i < argc; ++i )
	{
		const std::string arg = argv[ i ];
		const bool hasNext    = i + 1 < argc;
		if( arg == "--help" )
		{
			usage();
			return 0;
		}
		else if( arg == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( arg == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( arg == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( arg == "--level" && hasNext )
			level = std::atoi( argv[ ++i ] );
		else if( arg == "--audio" && hasNext )
			audioLevel = std::strtof( argv[ ++i ], nullptr );
		else if( arg == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( arg == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( arg == "--bench-frames" && hasNext )
			benchFrames = std::atoi( argv[ ++i ] );
		else if( arg == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( arg == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( arg == "--list" )
			wantList = true;
		else if( arg == "--bench" )
			wantBench = true;
		else if( arg == "--pipe" )
			wantPipe = true;
		else if( arg == "--allow-no-gl" )
			allowNoGL = true;
		else if( arg == "--quiet" )
			g_quiet = true;
		else if( arg.rfind( "--", 0 ) == 0 )
			modes.push_back( arg );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", arg.c_str() );
			usage();
			return 2;
		}
	}
	if( width <= 0 || height <= 0 || frames <= 0 )
	{
		std::fprintf( stderr, "width, height and frames must all be positive\n" );
		return 2;
	}

	if( wantList )
	{
		Downlink plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}
	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	//--offline is every check that needs no GL context, defined HERE, in the
	//one place that knows which those are: a GitHub macOS runner cannot create
	//an accelerated context, and a workflow that listed the groups itself
	//would go stale the first time one was added.
	if( !modes.empty() )
	{
		std::vector< std::string > expanded;
		for( const std::string& m : modes )
			if( m == "--offline" )
				for( const char* o : { "--names", "--clock", "--emphasis-offline", "--rice", "--negative-offline" } )
					expanded.push_back( o );
			else
				expanded.push_back( m );
		modes = expanded;

		bool needGL = false;
		for( const std::string& m : modes )
		{
			if( m == "--names" )
				runNames();
			else if( m == "--clock" )
				runClock();
			else if( m == "--emphasis-offline" )
				runEmphasisOffline();
			else if( m == "--rice" )
				runRice();
			else if( m == "--negative-offline" )
			{
				runNegativeCases( offlineNegatives() );
				std::printf( "\n   OFFLINE: nothing here drew a pixel through a GL driver. The link, the clamp, the\n"
				             "   decoder and every GL check and GL negative control were NOT run; in CI the shaders\n"
				             "   were only compiled, by glslc.\n" );
			}
			else if( m == "--emphasis" || m == "--clicks" || m == "--polarity" || m == "--threshold" || m == "--dispersal"
			         || m == "--identity" || m == "--seed" || m == "--resize" || m == "--negative" )
				needGL = true;
			else
			{
				std::fprintf( stderr, "unknown mode '%s'\n", m.c_str() );
				return 2;
			}
			std::printf( "\n" );
		}
		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         Every GL check and GL negative control was NOT run.\n\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				std::printf( "GL %s / %s, output raster %dx%d\n\n", glGetString( GL_VERSION ), glGetString( GL_RENDERER ),
				             width, height );
				for( const std::string& m : modes )
				{
					if( m == "--emphasis" )
						runEmphasis( width, height );
					else if( m == "--clicks" )
						runClicks( width, height );
					else if( m == "--polarity" )
						runPolarity( width, height );
					else if( m == "--threshold" )
						runThreshold( width, height );
					else if( m == "--dispersal" )
						runDispersal( width, height );
					else if( m == "--identity" )
						runIdentity( width, height );
					else if( m == "--seed" )
						runSeed( width, height );
					else if( m == "--resize" )
						runResize( width, height );
					else if( m == "--negative" )
					{
						runNegativeCases( offlineNegatives() );
						runNegativeCases( glNegatives( width, height ) );
					}
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( wantBench )
		return finish( runBench( settings, benchFrames ) );

	Session session;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( session.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return finish( 2 );
		}
	}
	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		const int status = runPipe( session, scriptPath, audioLevel );
		session.end();
		return finish( status );
	}

	const Image source = sourceName == "flat"        ? buildFlat( width, height, level )
	                     : sourceName == "quadrants" ? buildQuadrants( width, height )
	                                                 : buildCard( width, height );
	for( int frame = 0; frame < frames; ++frame )
	{
		if( audioLevel >= 0.0f )
			injectSpectrum( session.plugin, audioLevel, frame / 60.0 );
		if( !session.render( frame, source ) )
			return finish( 1 );
	}
	const Image image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
