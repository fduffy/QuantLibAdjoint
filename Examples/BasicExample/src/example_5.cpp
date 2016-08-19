// runExample_5 implementation

#include "example_5.hpp"
#include "utilities.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/swap/euriborswap.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>
#include <boost/timer/timer.hpp>

#include <iostream>
#include <fstream>

using namespace QuantLib;
using std::vector;
using std::map;
using std::string;
using std::cout;
using std::ofstream;
using std::endl;
using boost::timer::cpu_timer;
using boost::timer::cpu_times;

void runExample_5() {

	// Create a timer
	cpu_timer timer;
	timer.stop();

	// Set evaluation date
	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;

	// This will be the X (independent) vector.
	// 1 deposit rate, 2 FRAs and 5 swaps
	vector<Rate> marketRates{ 0.0100, 0.0125, 0.0150, 0.0300, 0.0350, 0.0400, .04500, 0.0550 };

	// Some indexing and conventions
	Size frasStart = 1;
	Size swapsStart = 3;
	vector<Natural> fraStartMonths{ 6, 12 };
	vector<Period> swapTenors{ 2 * Years, 5 * Years, 7 * Years, 10 * Years, 20 * Years };

	// Print out the header
	cout << "Portfolio Size,Pricing(s),Jacobian(s),One-sided(s),Two-sided(s),Tape Size(B)\n";

	for (Size k = 0; k < 19; ++k) {
#ifdef QL_ADJOINT
		// Start taping with zeroRates as independent variable
		cl::Independent(marketRates);
#endif

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
				fraStartMonths[i - frasStart], iborIndex));
		}

		// 3) Swaps
		boost::shared_ptr<SwapIndex> swapIndex;
		for (Size i = swapsStart; i < marketRates.size(); ++i) {
			marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
			swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i - swapsStart]);
			rateHelpers.push_back(boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex));
		}

		// Create yield curve and call discount to force a bootstrap
		Handle<YieldTermStructure> yieldCurve(
			boost::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(referenceDate, rateHelpers, dayCounter));
		yieldCurve->discount(0.5);

		// Create portfolio of swaps
		Size nSwaps;
		iborIndex = boost::make_shared<Euribor6M>(yieldCurve);

		// nSwaps 10, 20,..., 100, 200,..., 1000
		if (k < 10)
			nSwaps = (k + 1) * 10;
		else
			nSwaps = (k - 8) * 100;

		vector<boost::shared_ptr<VanillaSwap>> portfolio = makePortfolio(nSwaps, 15 * Years, iborIndex);

		// Price portfolio and time
		vector<Real> swapNpv(nSwaps, 0.0);
		timer.start();
		for (Size i = 0; i < nSwaps; ++i) {
			swapNpv[i] = portfolio[i]->NPV();
		}
		timer.stop();
		cout << nSwaps << "," << format(timer.elapsed(), 6, "%w");

#ifdef QL_ADJOINT
		// Stop taping and transfer operation sequence to function f (ultimately an AD function object)
		cl::tape_function<double> f(marketRates, swapNpv);
#endif

		// Calculate and time d(swapNpv_j) / d(q_i) for i = 1,..., nQuotes and j = 1,..., nSwaps
		Size nQuotes = marketRates.size();
		vector<double> jac(nSwaps * nQuotes, 0.0);
#ifdef QL_ADJOINT
		// Point at which to evaluate Jacobian = original market rates vector (but doubles)
		vector<double> x_0(nQuotes);
		for (Size j = 0; j < nQuotes; ++j) {
			x_0[j] = Value(marketRates[j].value());
		}
		timer.start();
		jac = f.Jacobian(x_0);
		timer.stop();
		cout << "," << format(timer.elapsed(), 6, "%w");
#else
		cout << ",0";
#endif
		// Calculate the derivatives by one-sided finite difference
		Real basisPoint = 0.0001;
		vector<Real> oneSidedDiffs(nSwaps * nQuotes, 0.0);
		timer.start();
		for (Size j = 0; j < nQuotes; ++j) {
			// Up 1 bp
			marketQuotes[j]->setValue(marketRates[j] + basisPoint);
			for (Size i = 0; i < nSwaps; ++i) {
				oneSidedDiffs[i * nQuotes + j] = (portfolio[i]->NPV() - swapNpv[i]) / basisPoint;
			}
			// Reset to original curve
			marketQuotes[j]->setValue(marketRates[j]);
		}
		timer.stop();
		cout << "," << format(timer.elapsed(), 6, "%w");

		// Calculate the derivatives by one-sided finite difference
		// Could re-use one-sided derivs above but do it again for timings
		vector<Real> twoSidedDiffs(nSwaps * nQuotes, 0.0);
		vector<Real> upNpv(nSwaps, 0.0);
		timer.start();
		for (Size j = 0; j < nQuotes; ++j) {
			// Up 1 bp
			marketQuotes[j]->setValue(marketRates[j] + basisPoint);
			for (Size i = 0; i < nSwaps; ++i) {
				upNpv[i] = portfolio[i]->NPV();
			}
			// Down 1 bp
			marketQuotes[j]->setValue(marketRates[j] - basisPoint);
			for (Size i = 0; i < nSwaps; ++i) {
				twoSidedDiffs[i * nQuotes + j] = (upNpv[i] - portfolio[i]->NPV()) / 2.0 / basisPoint;
			}
			// Reset to original curve
			marketQuotes[j]->setValue(marketRates[j]);
		}
		timer.stop();
		cout << "," << format(timer.elapsed(), 6, "%w");

#ifdef QL_ADJOINT
		cout << "," << f.size_op_seq() << "\n";
#else
		cout << ",0\n";
#endif

		// Output the results to file
		//if (nSwaps == 1000) {
		//	Size idx = 0;
		//	string filename = "../output/portfolio_" + std::to_string(nSwaps) + ".txt";
		//	ofstream ofs(filename);
		//	QL_REQUIRE(ofs.is_open(), "Could not open file " << filename);

		//	// ...header
		//	boost::format fmter("%s,%s,%s,%s");
		//	ofs << fmter % "Derivative" % "Jacobian" % "One FD" % "Two FD" << endl;

		//	// ...table
		//	fmter = boost::format("dV_%d/dq_%d,%.8f,%.8f,%.8f");
		//	for (Size i = 0; i < nSwaps; ++i) {
		//		for (Size j = 0; j < nQuotes; ++j) {
		//			idx = i * nQuotes + j;
		//			ofs << fmter % i % j % jac[idx] % oneSidedDiffs[idx] % twoSidedDiffs[idx] << endl;
		//		}
		//	}
		//}
		
	}
}
