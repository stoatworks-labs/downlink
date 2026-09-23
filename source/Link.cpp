#include "Link.h"

#include <algorithm>
#include <cmath>

namespace downlink::link
{
namespace
{
constexpr double kPi  = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;

/// F.405's zero and pole, from B and C: the curve is the magnitude of
/// g ( 1 + j f sqrt( C ) ) / ( 1 + j f sqrt( B ) ), f in MHz, so the time
/// constants are sqrt( C ) / 2 pi and sqrt( B ) / 2 pi microseconds.
double tauZeroSeconds()
{
	return std::sqrt( kPreC ) / kTau * 1e-6;
}
double tauPoleSeconds()
{
	return std::sqrt( kPreB ) / kTau * 1e-6;
}

/// Taps of a causal first-order section until the tail is below `floor` of
/// the first tap, capped.
std::vector< double > firstOrderTaps( const FirstOrder& n, int cap, double floor )
{
	std::vector< double > taps;
	taps.push_back( n.gain );
	double tail = n.gain * ( n.pole - n.zero );
	while( static_cast< int >( taps.size() ) < cap )
	{
		taps.push_back( tail );
		tail *= n.pole;
		if( std::abs( tail ) < floor * std::abs( n.gain ) )
			break;
	}
	return taps;
}
} // namespace

double F405Db( double fMHz )
{
	const double f2 = fMHz * fMHz;
	return 10.0 * std::log10( ( 1.0 + kPreC * f2 ) / ( 1.0 + kPreB * f2 ) ) - kPreA;
}

double F405ToleranceDb( double fMHz, double fcMHz )
{
	return 0.1 + 0.05 * fMHz / fcMHz;
}

double PreDcGain()
{
	return std::pow( 10.0, -kPreA / 20.0 );
}

//Matched z: the analogue zero and pole mapped by z = exp( s T ), and the
//gain set so DC is exactly the recommendation's -A dB. At 35.5 MHz this sits
//within 0.02 dB of F.405 from 0.01 to 5 MHz (the tolerance is 0.1 dB); a
//bilinear transform does not (0.18 dB at 5 MHz, prewarped or not).
FirstOrder PreEmphasisNetwork( double fs )
{
	const double T    = 1.0 / fs;
	const double zero = std::exp( -T / tauZeroSeconds() );
	const double pole = std::exp( -T / tauPoleSeconds() );
	const double gain = PreDcGain() * ( 1.0 - pole ) / ( 1.0 - zero );
	return { gain, zero, pole };
}

FirstOrder DeEmphasisNetwork( double fs )
{
	const double T    = 1.0 / fs;
	const double zero = std::exp( -T / tauPoleSeconds() );
	const double pole = std::exp( -T / tauZeroSeconds() );
	const double gain = ( 1.0 / PreDcGain() ) * ( 1.0 - pole ) / ( 1.0 - zero );
	return { gain, zero, pole };
}

std::vector< double > PreEmphasisTaps( double fs )
{
	return firstOrderTaps( PreEmphasisNetwork( fs ), kMaxPreTaps, 1e-7 );
}

std::vector< double > DeEmphasisTaps( double fs )
{
	return firstOrderTaps( DeEmphasisNetwork( fs ), kMaxDeTaps, 1e-6 );
}

std::vector< double > VideoLowpassTaps( double cutoffHz, double fs )
{
	const double fc = cutoffHz / fs;//cycles per sample
	std::vector< double > taps( 2 * kLpfHalf + 1 );
	double sum = 0.0;
	for( int i = -kLpfHalf; i <= kLpfHalf; ++i )
	{
		const double x = 2.0 * fc * i;
		const double s = i == 0 ? 1.0 : std::sin( kPi * x ) / ( kPi * x );
		const double w = 0.5 + 0.5 * std::cos( kPi * i / ( kLpfHalf + 1.0 ) );
		taps[ static_cast< size_t >( i + kLpfHalf ) ] = s * w;
		sum += s * w;
	}
	for( double& t : taps )
		t /= sum;
	return taps;
}

double IfSigmaHz( double bandwidthHz )
{
	return bandwidthHz / ( 2.0 * std::sqrt( 2.0 * std::log( 2.0 ) ) );
}

//|H|^2 = exp( -f^2 / 2 sigma^2 ) means |H| = exp( -f^2 / 4 sigma^2 ), whose
//impulse response is a Gaussian of sigma_t = 1 / ( 2 pi sqrt( 2 ) sigma ).
std::vector< double > NoiseTaps( double sigmaHz, double fsFine )
{
	const double sigmaT = fsFine / ( kTau * std::sqrt( 2.0 ) * sigmaHz );//in fine samples
	const int half      = std::min( kMaxNoiseHalf, static_cast< int >( std::ceil( 4.0 * sigmaT ) ) );
	std::vector< double > taps( static_cast< size_t >( 2 * half + 1 ) );
	double energy = 0.0;
	for( int i = -half; i <= half; ++i )
	{
		const double v                             = std::exp( -0.5 * ( i / sigmaT ) * ( i / sigmaT ) );
		taps[ static_cast< size_t >( i + half ) ] = v;
		energy += v * v;
	}
	const double norm = 1.0 / std::sqrt( energy );
	for( double& t : taps )
		t *= norm;
	return taps;
}

double Triangle( double cycleSeconds, double ppMHz )
{
	const double u = cycleSeconds * kDispersalHz - std::floor( cycleSeconds * kDispersalHz );
	return u < 0.5 ? ppMHz * ( 2.0 * u - 0.5 ) : ppMHz * ( 1.5 - 2.0 * u );
}

int64_t FieldIndex( double hostSeconds )
{
	return static_cast< int64_t >( std::floor( hostSeconds / kFieldSeconds ) );
}

//The field on air at the host's time, and every row's place in the 40 ms
//triangle cycle. The field index is a huge number on a real host (Resolume's
//clock has been seen at 499,217 s, 25 million fields), so only its parity
//crosses into the cycle, and nothing absolute reaches a float.
void MakeRowTable( double hostSeconds, double dispersalPpMHz, RowTable& table )
{
	table.field  = FieldIndex( hostSeconds );
	table.parity = static_cast< int >( ( ( table.field % 2 ) + 2 ) % 2 );

	const int n = kHistoryRows + kRows;
	table.base.assign( static_cast< size_t >( n ), 0.0f );
	table.slope.assign( static_cast< size_t >( n ), 0.0f );
	table.cycleSeconds.assign( static_cast< size_t >( n ), 0.0 );

	const double fieldStart = table.parity * kFieldSeconds;
	const double perSample  = 1.0 / kFsVideo;
	for( int i = 0; i < n; ++i )
	{
		const int row    = i - kHistoryRows;
		double t         = fieldStart + kFirstRowSeconds + row * kRowSeconds;
		const double cyc = 2.0 * kFieldSeconds;
		t -= std::floor( t / cyc ) * cyc;
		table.cycleSeconds[ static_cast< size_t >( i ) ] = t;
		const double v0 = Triangle( t, dispersalPpMHz );
		//The slope is the triangle's, which never turns inside a row: the
		//turn is at the field boundary and the last row ends 0.07 ms before it.
		const double u  = t * kDispersalHz - std::floor( t * kDispersalHz );
		const double ds = ( u < 0.5 ? 2.0 : -2.0 ) * dispersalPpMHz * kDispersalHz * perSample;
		table.base[ static_cast< size_t >( i ) ]  = static_cast< float >( v0 );
		table.slope[ static_cast< size_t >( i ) ] = static_cast< float >( ds );
	}
}

//PAL: 283.7516 cycles of subcarrier in a 64 us line.
double SubcarrierStepPerLinkSample()
{
	return kTau * kFsc / kFsLink;//pi / 4
}

double SubcarrierStepPerRow()
{
	double whole = 0.0;
	return kTau * std::modf( 283.7516, &whole );
}

double SubcarrierPhaseForField( int64_t field )
{
	//283.7516 x 312.5 cycles a field; only the fraction matters, and the
	//product is taken modulo 1 in two steps so a 25-million-field count does
	//not lose the fraction.
	const double perField = std::fmod( 283.7516 * 312.5, 1.0 );
	const double n        = static_cast< double >( field % 1000000 );
	return kTau * std::fmod( perField * n, 1.0 );
}

} // namespace downlink::link
