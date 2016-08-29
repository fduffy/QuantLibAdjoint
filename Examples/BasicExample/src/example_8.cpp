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
	vector<boost::shared_ptr<RateHelper>> rateHelpers;
	vector<boost::shared_ptr<SimpleQuote>> marketQuotes;

#ifdef QL_ADJOINT
	// Start taping with discounts as independent variable
	cl::Independent(marketRates);
#endif

	boost::shared_ptr<SwapIndex> swapIndex;
	for (Size i = 0; i < nQuotes; ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i]);
		rateHelpers.push_back(boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex));
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
	// Stop taping
	cl::tape_function<double> fnDiscounts(marketRates, discounts);
#endif

	// Calculate the Jacobian of discounts wrt input rates
	// Want to avoid this
	vector<double> jacDiscounts(nQuotes * nDiscounts, 0.0);
#ifdef QL_ADJOINT
	vector<double> marketRates_0(nQuotes);
	for (Size j = 0; j < nQuotes; ++j) {
		marketRates_0[j] = Value(marketRates[j].value());
	}
	jacDiscounts = fnDiscounts.Jacobian(marketRates_0);
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
	for (Size i = 0; i < nQuotes; ++i) {
		rateHelpers[i]->setTermStructure((*yts).get());
		boost::shared_ptr<SwapRateHelper> swapRateHelper = boost::dynamic_pointer_cast<SwapRateHelper>(rateHelpers[i]);
		swapFairRates[i] = swapRateHelper->swap()->fairRate();
	}

#ifdef QL_ADJOINT
	// Stop taping
	cl::tape_function<double> rates(discounts, swapFairRates);
#endif

	// Calculate the Jacobian of input instrument fair rates wrt zero rates and invert
	vector<double> jacRates(nQuotes * nDiscounts, 0.0);
#ifdef QL_ADJOINT
	vector<double> x_0(nDiscounts);
	for (Size j = 0; j < nDiscounts; ++j) {
		x_0[j] = Value(discounts[j].value());
	}
	jacRates = rates.Jacobian(x_0);
#endif

	// Calculate Jacobian of portfolio wrt discount factors
#ifdef QL_ADJOINT
	// Start taping with discounts as independent variable
	cl::Independent(discounts);
#endif

	// Create the discount curve
	tempDiscounts = discounts;
	tempDiscounts.insert(tempDiscounts.begin(), 1.0);
	yts.linkTo(boost::make_shared<DiscountCurve>(dates, tempDiscounts, dayCounter));

	// Create portfolio of swaps linked to discount curve
	Size nSwaps = 10;
	boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>(yts);
	vector<boost::shared_ptr<VanillaSwap>> portfolio = makePortfolio(nSwaps, 15 * Years, iborIndex);

	// Price portfolio
	vector<Real> swapNpv(nSwaps, 0.0);
	for (Size i = 0; i < nSwaps; ++i) {
		swapNpv[i] = portfolio[i]->NPV();
	}

#ifdef QL_ADJOINT
	// Stop taping
	cl::tape_function<double> npv(discounts, swapNpv);
#endif

	// Calculate and time d(swapNpv_j) / d(df_i) for i = 1,..., nDiscounts and j = 1,..., nSwaps
	vector<double> jacNpv(nSwaps * nDiscounts, 0.0);
#ifdef QL_ADJOINT
	// Point at which to evaluate Jacobian = original discount factors
	for (Size j = 0; j < nDiscounts; ++j) {
		x_0[j] = Value(discounts[j].value());
	}
	jacNpv = npv.Jacobian(x_0);
#endif

	// Calculate the derivatives by one-sided finite difference
	yts.linkTo(yieldCurve);
	Real basisPoint = 0.0001;
	vector<Real> oneSidedDiffs(nSwaps * nQuotes, 0.0);
	for (Size j = 0; j < nQuotes; ++j) {
		// Up 1 bp
		marketQuotes[j]->setValue(marketRates[j] + basisPoint);
		for (Size i = 0; i < nSwaps; ++i) {
			oneSidedDiffs[i * nQuotes + j] = (portfolio[i]->NPV() - swapNpv[i]) / basisPoint;
		}
		// Reset to original curve
		marketQuotes[j]->setValue(marketRates[j]);
	}

	// Calculate the derivatives by two-sided finite difference
	// Could re-use one-sided derivs above but do it again for timings
	vector<Real> twoSidedDiffs(nSwaps * nQuotes, 0.0);
	vector<Real> upNpv(nSwaps, 0.0);
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

	// Output the results
	Size idx = 0;
	boost::format fmter = boost::format(" %+.7f ");

	cout << "d df / d InputRates:\n";
	for (Size i = 0; i < nDiscounts; ++i) {
		cout << "  |";
		for (Size j = 0; j < nQuotes; ++j) {
			idx = i * nQuotes + j;
			cout << fmter % jacDiscounts[idx];
		}
		cout << "|\n";
	}
	cout << "\n";

	cout << "d InputRates / d df:\n";
	for (Size i = 0; i < nQuotes; ++i) {
		cout << "  |";
		for (Size j = 0; j < nDiscounts; ++j) {
			idx = i * nDiscounts + j;
			cout << fmter % jacRates[idx];
		}
		cout << "|\n";
	}
	cout << "\n";

	cout << "d SwapValues / d df:\n";
	for (Size i = 0; i < nSwaps; ++i) {
		cout << "  |";
		for (Size j = 0; j < nDiscounts; ++j) {
			idx = i * nDiscounts + j;
			cout << fmter % jacNpv[idx];
		}
		cout << "|\n";
	}
	cout << "\n";
}
