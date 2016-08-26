// runExample_8 implementation

#include "example_8.hpp"
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

void runExample_8() {

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

#ifdef QL_ADJOINT
	// Start taping with discounts as independent variable
	cl::Independent(marketRates);
#endif

	boost::shared_ptr<SwapIndex> swapIndex;
	for (Size i = 0; i < nQuotes; ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i]);
		temp = boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex);
		rateHelpers.push_back(temp);
		inputSwaps.push_back(temp->swap());
	}

	// Create yield curve
	boost::shared_ptr<PiecewiseYieldCurve<Discount, LogLinear>> yieldCurve(
		boost::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(referenceDate, rateHelpers, dayCounter));

	// Create discount factor curve with discounts as independent variables for AD
	vector<Real> discounts = yieldCurve->data();
	vector<Date> dates = yieldCurve->dates();
	// Remove first element i.e. 1.0 from the discount vector
	discounts.erase(discounts.begin());
	Size nDiscounts = discounts.size();

#ifdef QL_ADJOINT
	// Stop taping and transfer operation sequence to function fnDiscounts (ultimately an AD function object)
	cl::tape_function<double> fnDiscounts(marketRates, discounts);
#endif

	// Calculate the Jacobian of discounts wrt input rates
	vector<double> jacDiscounts(nQuotes * nDiscounts, 0.0);
#ifdef QL_ADJOINT
	// Point at which to evaluate Jacobian = ??
	vector<double> marketRates_0(nQuotes);
	for (Size j = 0; j < nQuotes; ++j) {
		marketRates_0[j] = Value(marketRates[j].value());
	}
	timer.start();
	jacDiscounts = fnDiscounts.Jacobian(marketRates_0);
	timer.stop();
#endif

#ifdef QL_ADJOINT
	// Start taping with discounts as independent variable
	cl::Independent(discounts);
#endif

	// Create the discount curve
	vector<Real> tempDiscounts = discounts;
	tempDiscounts.insert(tempDiscounts.begin(), 1.0);
	boost::shared_ptr<DiscountCurve> discountCurve = 
		boost::make_shared<DiscountCurve>(dates, tempDiscounts, dayCounter);
	yts.linkTo(discountCurve);
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
	// Stop taping and transfer operation sequence to function rates (ultimately an AD function object)
	cl::tape_function<double> rates(discounts, swapFairRates);
#endif

	// Calculate the Jacobian of input instrument fair rates wrt zero rates and invert
	vector<double> jac(nQuotes * nDiscounts, 0.0);
#ifdef QL_ADJOINT
	// Point at which to evaluate Jacobian = ??
	vector<double> x_0(nDiscounts);
	for (Size j = 0; j < nDiscounts; ++j) {
		x_0[j] = Value(discounts[j].value());
	}
	timer.start();
	jac = rates.Jacobian(x_0);
	timer.stop();
#endif

	// Print out the header
	cout << "Portfolio Size,Pricing(s),Jacobian(s),One-sided(s),Two-sided(s),Tape Size(B)\n";

	for (Size k = 0; k < 1; ++k) {
#ifdef QL_ADJOINT
		// Start taping with discounts as independent variable
		cl::Independent(discounts);
#endif

		// Create the discount curve
		vector<Real> tempDiscounts = discounts;
		tempDiscounts.insert(tempDiscounts.begin(), 1.0);
		yts.linkTo(boost::make_shared<DiscountCurve>(dates, tempDiscounts, dayCounter));

		// Create portfolio of swaps linked to discount curve
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
		// Stop taping and transfer operation sequence to function npv (ultimately an AD function object)
		cl::tape_function<double> npv(discounts, swapNpv);
#endif

		// Calculate and time d(swapNpv_j) / d(df_i) for i = 1,..., nDiscounts and j = 1,..., nSwaps
		vector<double> jac(nSwaps * nDiscounts, 0.0);
#ifdef QL_ADJOINT
		// Point at which to evaluate Jacobian = original zero rates vector (but doubles)
		vector<double> x_0(nDiscounts);
		for (Size j = 0; j < nDiscounts; ++j) {
			x_0[j] = Value(discounts[j].value());
		}
		timer.start();
		jac = npv.Jacobian(x_0);
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
		cout << "," << npv.size_op_seq() << "\n";
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
