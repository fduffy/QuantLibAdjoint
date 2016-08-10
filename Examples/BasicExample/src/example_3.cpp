// runExample_3 implementation

#include "example_3.hpp"
#include "utilities.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/swap/euriborswap.hpp>
#include <ql/instruments/makevanillaswap.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>
#include <boost/timer/timer.hpp>

#include <iostream>

using namespace QuantLib;
using std::vector;
using std::map;
using std::string;
using std::cout;
using std::endl;
using boost::timer::cpu_timer;
using boost::timer::cpu_times;

void runExample_3() {

	// Create a timer
	cpu_timer timer;
	timer.stop();
	map<string, cpu_times> timings;

	// Set evaluation date
	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;

	// These will be the X (independent) and Y (dependent) vectors.
	// 1 deposit rate, 2 FRAs and 5 swaps
	vector<Rate> marketRates{ 0.0100, 0.0125, 0.0150, 0.0300, 0.0350, 0.0400, .04500, 0.0550 };
	vector<Rate> swapNpv(1, 0.0);

	// Some indexing and conventions
	Size frasStart = 1;
	Size swapsStart = 3;
	vector<Natural> fraStartMonths{ 6, 12 };
	vector<Period> swapTenors{ 2*Years, 5*Years, 7*Years, 10*Years, 20*Years };

	// Start taping with zeroRates as independent variable
	cl::Independent(marketRates);

	// Set up bootstrapped yield curve
	vector<boost::shared_ptr<RateHelper>> rateHelpers;
	vector<boost::shared_ptr<SimpleQuote>> marketQuotes;

	// 1) Deposits
	boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>();
	marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[0]));
	rateHelpers.push_back(boost::make_shared<DepositRateHelper>(Handle<Quote>(marketQuotes[0]), iborIndex));

	// 2) FRAs
	for (Size i = frasStart; i < swapsStart; ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		rateHelpers.push_back(boost::make_shared<FraRateHelper>(Handle<Quote>(marketQuotes[i]), 
			fraStartMonths[i-frasStart], iborIndex));
	}

	// 3) Swaps
	boost::shared_ptr<SwapIndex> swapIndex;
	for (Size i = swapsStart; i < marketRates.size(); ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i-swapsStart]);
		rateHelpers.push_back(boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex));
	}

	// Create yield curve
	Handle<YieldTermStructure> yieldCurve(
		boost::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(referenceDate, rateHelpers, dayCounter));

	// Create swap
	Period swapTenor(15, Years);
	iborIndex = boost::make_shared<Euribor6M>(yieldCurve);
	Rate fixedRate = 0.0325;
	Period forwardStart(0, Days);
	boost::shared_ptr<VanillaSwap> swap = MakeVanillaSwap(swapTenor, iborIndex, fixedRate, forwardStart);

	// Price swap and time
	timer.start();
	swapNpv[0] = swap->NPV();
	timer.stop();
	timings["pricing"] = timer.elapsed();

	// Stop taping and transfer operation sequence to function f (ultimately an AD function object)
	cl::tape_function<double> f(marketRates, swapNpv);

	// Calculate and time d(swapNpv) / d(z_i) for i = 1, ..., nZeros
	// ... with forward mode
	Size nQuotes = marketRates.size();
	vector<double> dZ(nQuotes, 0.0);
	vector<double> forwardDerivs(nQuotes, 0.0);
	timer.start();
	for (Size i = 0; i < nQuotes; ++i) {
		dZ[i] = 1.0;
		forwardDerivs[i] = f.Forward(1, dZ)[0];
		dZ[i] = 0.0;

	}
	timer.stop();
	timings["forward"] = timer.elapsed();
	
	// ... with reverse mode
	timer.start();
	vector<double> reverseDerivs = f.Reverse(1, vector<double>(1, 1.0));
	timer.stop();
	timings["reverse"] = timer.elapsed();

	// Calculate the derivatives by one-sided finite difference
	Real basisPoint = 0.0001;
	vector<Real> oneSidedDiffs(nQuotes, 0.0);
	timer.start();
	for (Size i = 0; i < nQuotes; ++i) {
		// Up 1 bp
		marketQuotes[i]->setValue(marketRates[i] + basisPoint);
		oneSidedDiffs[i] = (swap->NPV() - swapNpv[0]) / basisPoint;
		// Reset to original curve
		marketQuotes[i]->setValue(marketRates[i]);
	}
	timer.stop();
	timings["one-sided FD"] = timer.elapsed();

	// Calculate the derivatives by one-sided finite difference
	// Could re-use one-sided derivs above but do it again for timings
	vector<Real> twoSidedDiffs(nQuotes, 0.0);
	Real upNpv = 0.0;
	timer.start();
	for (Size i = 0; i < nQuotes; ++i) {
		// Up 1 bp
		marketQuotes[i]->setValue(marketRates[i] + basisPoint);
		upNpv = swap->NPV();
		// Down 1 bp
		marketQuotes[i]->setValue(marketRates[i] - basisPoint);
		twoSidedDiffs[i] = (upNpv - swap->NPV()) / 2.0 / basisPoint;
		// Reset to original curve
		marketQuotes[i]->setValue(marketRates[i]);
	}
	timer.stop();
	timings["two-sided FD"] = timer.elapsed();

	// Output the results
	boost::format headerFmter("%-12s|%=16s|%=16s|%=16s|%=16s");
	boost::format fmter("quote_%-6s|%=+16.8f|%=+16.8f|%=+16.8f|%=+16.8f");
	
	cout << endl;
	cout << "Compare derivatives:" << endl;
	cout << endl;
	headerFmter % "Quote" % "Forward" % "Reverse" % "One FD" % "Two FD";
	cout << headerFmter;
	string rule(headerFmter.str().length(), '=');
	cout << endl << rule << endl;
	for (Size i = 0; i < nQuotes; ++i) {
		cout << fmter % (i + 1) % forwardDerivs[i] % reverseDerivs[i] % oneSidedDiffs[i] % twoSidedDiffs[i] << endl;
	}
	cout << endl;

	// Output the timings in a table
	printTimings(timings);

	// Output some properties of the tape sequence
	printProperties<double>(f);
}
