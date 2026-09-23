#pragma once

#include <algorithm>
#include <cmath>

/**
	What a 0..1 host slider means, in MHz, dB and MHz/V.

	Every ranged host parameter is 0..1: `SetParamInfo` clamps an
	FF_TYPE_STANDARD default into 0..1 before `SetParamRange` can widen it.
	Each mapping has its inverse so the constructor can say its defaults in
	physical units, and so the harness can set a physical value by name.

	Options are mapped by index here and nowhere else: an option parameter's
	range reads back 0..1 whatever its element count.
*/
namespace downlink::controls
{
inline float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}

inline float linear( float p, float lo, float hi )
{
	return lo + ( hi - lo ) * clamp01( p );
}

inline float inverseLinear( float v, float lo, float hi )
{
	return clamp01( ( v - lo ) / ( hi - lo ) );
}

//--- Uplink ----------------------------------------------------------------
/// Peak-to-peak deviation for 1 V p-p at the pre-emphasis crossover, MHz/V.
/// Intelsat used 13.5, Astra 16; F.405's radio-relay figure is 8.
constexpr float kDeviationLo = 4.0f, kDeviationHi = 24.0f;
inline float DeviationMHzPerVolt( float p )
{
	return linear( p, kDeviationLo, kDeviationHi );
}
inline float DeviationParam( float v )
{
	return inverseLinear( v, kDeviationLo, kDeviationHi );
}

/// Energy dispersal, MHz peak-to-peak (2 MHz on a shared transponder).
constexpr float kDispersalHi = 8.0f;
inline float DispersalMHz( float p )
{
	return linear( p, 0.0f, kDispersalHi );
}
inline float DispersalParam( float v )
{
	return inverseLinear( v, 0.0f, kDispersalHi );
}

//--- Link --------------------------------------------------------------------
/// Carrier-to-noise in the IF, dB.
constexpr float kCnrLo = 0.0f, kCnrHi = 25.0f;
inline float CnrDb( float p )
{
	return linear( p, kCnrLo, kCnrHi );
}
inline float CnrParam( float v )
{
	return inverseLinear( v, kCnrLo, kCnrHi );
}

constexpr float kRainHi = 10.0f;
inline float RainFadeDb( float p )
{
	return linear( p, 0.0f, kRainHi );
}
inline float RainFadeParam( float v )
{
	return inverseLinear( v, 0.0f, kRainHi );
}

/// Audio Fade: dB of fade at full level. The level is the RMS of the host's
/// bins, clamped to 1 -- total level only, because nobody has measured what
/// Resolume's 64 bins are.
constexpr float kAudioFadeHi = 12.0f;
inline float AudioFadeDb( float p, float level )
{
	return linear( p, 0.0f, kAudioFadeHi ) * std::clamp( level, 0.0f, 1.0f );
}

/// IF bandwidth at -3 dB, MHz. 27 is the full-transponder figure.
constexpr float kIfLo = 18.0f, kIfHi = 36.0f;
inline float IfBandwidthMHz( float p )
{
	return linear( p, kIfLo, kIfHi );
}
inline float IfBandwidthParam( float v )
{
	return inverseLinear( v, kIfLo, kIfHi );
}

//--- Receiver ------------------------------------------------------------------
constexpr float kVideoLo = 2.0f, kVideoHi = 6.0f;
inline float VideoBandwidthMHz( float p )
{
	return linear( p, kVideoLo, kVideoHi );
}
inline float VideoBandwidthParam( float v )
{
	return inverseLinear( v, kVideoLo, kVideoHi );
}

enum Demodulator
{
	kDiscriminator,
	kThresholdExtension,
	kDemodulatorCount
};
inline const char* DemodulatorName( int i )
{
	static const char* const names[] = { "Discriminator", "Threshold Ext." };
	return names[ std::clamp( i, 0, kDemodulatorCount - 1 ) ];
}

enum Clamp
{
	kClampOff,
	kClampSlow,
	kClampGood,
	kClampCount
};
inline const char* ClampName( int i )
{
	static const char* const names[] = { "Off", "Slow", "Good" };
	return names[ std::clamp( i, 0, kClampCount - 1 ) ];
}

//--- Signal --------------------------------------------------------------------
enum Composite
{
	kPal,
	kComponent,
	kCompositeCount
};
inline const char* CompositeName( int i )
{
	static const char* const names[] = { "PAL", "Component" };
	return names[ std::clamp( i, 0, kCompositeCount - 1 ) ];
}

inline int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace downlink::controls
