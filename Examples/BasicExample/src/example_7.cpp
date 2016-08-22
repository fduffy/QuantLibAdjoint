// runExample_7 implementation

#include "example_7.hpp"
#include "utilities.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/swap/euriborswap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
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

void runExample_7() {

	// Create a timer
	cpu_timer timer;
	timer.stop();

	// Set evaluation date
	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;
	RelinkableHandle<YieldTermStructure> yts;

	// Market rates: 6 swaps
	vector<Rate> marketRates { 0.020, 0.0300, 0.0350, 0.0400, .04500, 0.0550 };
	Size nQuotes = marketRates.size();

	// Some indexing and conventions
	vector<Period> swapTenors{ 1 * Years, 2 * Years, 5 * Years, 7 * Years, 10 * Years, 20 * Years };

	// Set up bootstrapped yield curve
	boost::shared_ptr<SwapRateHelper> temp;
	vector<boost::shared_ptr<RateHelper>> rateHelpers;
	vector<boost::shared_ptr<SimpleQuote>> marketQuotes;
	vector<boost::shared_ptr<VanillaSwap>> inputSwaps;

	boost::shared_ptr<SwapIndex> swapIndex;
	for (Size i = 0; i < nQuotes; ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i]);
		temp = boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex);
		rateHelpers.push_back(temp);
		inputSwaps.push_back(temp->swap());
	}

	// Create yield curve
	boost::shared_ptr<PiecewiseYieldCurve<ZeroYield, Linear>> yieldCurve(
		boost::make_shared<PiecewiseYieldCurve<ZeroYield, Linear>>(referenceDate, rateHelpers, dayCounter));

	// Create zero curve with zeroes as independent variables for AD
	vector<Rate> zeroes = yieldCurve->data();
	vector<Date> dates = yieldCurve->dates();
	Size nZeroes = zeroes.size();

#ifdef QL_ADJOINT
	// Start taping with zeroRates as independent variable
	cl::Independent(zeroes);
#endif

	// Create the zero curve
	boost::shared_ptr<ZeroCurve> zeroCurve = boost::make_shared<ZeroCurve>(dates, zeroes, dayCounter);
	yts.linkTo(zeroCurve);
	boost::shared_ptr<PricingEngine> engine = boost::make_shared<DiscountingSwapEngine>(yts);

	// Calculate the value of input instruments as a function of the zero rates
	vector<Real> swapFairRates(nQuotes, 0.0);
	timer.start();
	for (Size i = 0; i < nQuotes; ++i) {
		inputSwaps[i]->setPricingEngine(engine);
		swapFairRates[i] = inputSwaps[i]->fairRate();
	}
	timer.stop();

#ifdef QL_ADJOINT
	// Stop taping and transfer operation sequence to function h (ultimately an AD function object)
	cl::tape_function<double> h(zeroes, swapFairRates);
#endif

	// Calculate the Jacobian of input instrument fair rates wrt zero rates and invert
	vector<double> jac(nQuotes * nZeroes, 0.0);
#ifdef QL_ADJOINT
	// Point at which to evaluate Jacobian = ??
	vector<double> x_0(nZeroes);
	for (Size j = 0; j < nZeroes; ++j) {
		x_0[j] = Value(zeroes[j].value());
	}
	timer.start();
	jac = h.Jacobian(x_0);
	timer.stop();
#endif

	// Print out the header
	cout << "Portfolio Size,Pricing(s),Jacobian(s),One-sided(s),Two-sided(s),Tape Size(B)\n";

	for (Size k = 0; k < 1; ++k) {
#ifdef QL_ADJOINT
		// Start taping with zeroRates as independent variable
		cl::Independent(zeroes);
#endif

		// Create the zero curve
		yts.linkTo(boost::make_shared<ZeroCurve>(dates, zeroes, dayCounter));

		// Create portfolio of swaps linked to zero curve
		Size nSwaps;
		boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>(yts);

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
		cl::tape_function<double> f(zeroes, swapNpv);
#endif

		// Calculate and time d(swapNpv_j) / d(z_i) for i = 1,..., nZeroes and j = 1,..., nSwaps
		vector<double> jac(nSwaps * nZeroes, 0.0);
#ifdef QL_ADJOINT
		// Point at which to evaluate Jacobian = original zero rates vector (but doubles)
		vector<double> x_0(nZeroes);
		for (Size j = 0; j < nZeroes; ++j) {
			x_0[j] = Value(zeroes[j].value());
		}
		timer.start();
		jac = f.Jacobian(x_0);
		timer.stop();
		cout << "," << format(timer.elapsed(), 6, "%w");
#else
		cout << ",0";
#endif
		// Calculate the derivatives by one-sided finite difference
		yts.linkTo(yieldCurve);
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

		// Calculate the derivatives by two-sided finite difference
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
