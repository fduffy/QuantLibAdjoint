// runExample_9 implementation

#include "example_9.hpp"
#include "utilities.hpp"

#include <ql/cashflows/coupon.hpp>
#include <ql/exercise.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/swap/euriborswap.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/models/shortrate/onefactormodels/gsr.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/pricingengines/swaption/gaussian1dswaptionengine.hpp>
#include <ql/pricingengines/swaption/gaussian1djamshidianswaptionengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/calendars/target.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>
#include <boost/timer/timer.hpp>

#include <iostream>
#include <fstream>

using namespace QuantLib;
using std::list;
using std::vector;
using std::map;
using std::string;
using std::cout;
using std::ofstream;
using std::endl;
using boost::timer::cpu_timer;
using boost::timer::cpu_times;

void runExample_9() {

	// Create a timer
	cpu_timer timer;
	timer.stop();

	// Set evaluation date
	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;
	TARGET calendar;

	// Market rates: 6 swaps
	vector<Rate> marketRates{ 0.020, 0.0300, 0.0350, 0.0400, .04500, 0.0500, 0.0550 };
	Size nQuotes = marketRates.size();

	// Some indexing and conventions
	vector<Period> swapTenors{ 1 * Years, 2 * Years, 5 * Years, 7 * Years, 10 * Years, 20 * Years, 40 * Years };

	// Set up bootstrapped yield curve
	vector<boost::shared_ptr<RateHelper>> rateHelpers;
	vector<boost::shared_ptr<SimpleQuote>> marketQuotes;

	boost::shared_ptr<SwapIndex> swapIndex;
	for (Size i = 0; i < nQuotes; ++i) {
		marketQuotes.push_back(boost::make_shared<SimpleQuote>(marketRates[i]));
		swapIndex = boost::make_shared<EuriborSwapIsdaFixA>(swapTenors[i]);
		rateHelpers.push_back(boost::make_shared<SwapRateHelper>(Handle<Quote>(marketQuotes[i]), swapIndex));
	}

	// Create yield curve
	boost::shared_ptr<PiecewiseYieldCurve<Discount, LogLinear>> yieldCurve(
		boost::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(referenceDate, rateHelpers, dayCounter));
	Handle<YieldTermStructure> yts(yieldCurve);

	// Create Bermudan swaption instrument that we want to price
	// Option to enter a swap, ending on 3 Aug 2041, in 5Y and every 5Y thereafter
	// 1. Create underlying ATM swap
	Period fixedLegTenor = 1 * Years;
	BusinessDayConvention fixedLegConvention = Unadjusted;
	BusinessDayConvention floatingLegConvention = ModifiedFollowing;
	Thirty360 fixedLegDayCounter(Thirty360::European);
	Period floatingLegTenor = 6 * Months;
	VanillaSwap::Type type = VanillaSwap::Payer;
	Rate dummyFixedRate = 0.03;
	boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>(yts);
	Real nominal = 100000;

	Date startDate = calendar.advance(referenceDate, 5, Years, floatingLegConvention);
	Date maturity = calendar.advance(startDate, 20, Years, floatingLegConvention);
	Schedule fixedSchedule(startDate, maturity, fixedLegTenor, calendar,
		fixedLegConvention, fixedLegConvention, DateGeneration::Forward, false);
	Schedule floatSchedule(startDate, maturity, floatingLegTenor, calendar,
		floatingLegConvention, floatingLegConvention, DateGeneration::Forward, false);

	boost::shared_ptr<VanillaSwap> swap = boost::make_shared<VanillaSwap>(type, nominal, fixedSchedule, dummyFixedRate,
		fixedLegDayCounter, floatSchedule, iborIndex, 0.0, iborIndex->dayCounter());
	swap->setPricingEngine(boost::make_shared<DiscountingSwapEngine>(yts));
	Rate fixedAtmRate = swap->fairRate();

	swap = boost::make_shared<VanillaSwap>(type, nominal, fixedSchedule, fixedAtmRate, fixedLegDayCounter,
		floatSchedule, iborIndex, 0.0, iborIndex->dayCounter());

	// 2. Create the Bermudan swaption
	// ... bermudan dates are every 5 years => every 5th date on the fixed leg of underlying
	vector<Date> bermudanDates;
	const vector<boost::shared_ptr<CashFlow> >& leg = swap->fixedLeg();
	for (Size i = 0; i < leg.size() - 1; i += 5) {
		boost::shared_ptr<Coupon> coupon = boost::dynamic_pointer_cast<Coupon>(leg[i]);
		bermudanDates.push_back(coupon->accrualStartDate());
	}
	boost::shared_ptr<Exercise> bermudanExercise = boost::make_shared<BermudanExercise>(bermudanDates);
	Swaption bermudanSwaption(swap, bermudanExercise);

	// Create European swaptions that we will calibrate to
	// 1. European swaption volatilities from the market (diagonal for Bermudan swaption above)
	vector<Period> expiries{ 5 * Years, 10 * Years, 15 * Years, 20 * Years };
	vector<Period> tenors{ 20 * Years, 15 * Years, 10 * Years, 5 * Years };
	vector<Volatility> volatilities{ 0.18, 0.16, 0.14, 0.12 };

	// 2. Create the calibration helpers
	vector<boost::shared_ptr<CalibrationHelper>> swaptions(expiries.size());
	for (Size i = 0; i < expiries.size(); ++i) {
		boost::shared_ptr<Quote> vol = boost::make_shared<SimpleQuote>(volatilities[i]);
		swaptions[i] = boost::make_shared<SwaptionHelper>(expiries[i], tenors[i], Handle<Quote>(vol),
			iborIndex, fixedLegTenor, fixedLegDayCounter, iborIndex->dayCounter(), yts);
	}

	// Create the GSR interest rate model with dummy initial volatilities
	vector<Real> initialVols(bermudanDates.size() + 1, 0.01);
	Real reversion = 0.02;
	boost::shared_ptr<Gsr> gsr = boost::make_shared<Gsr>(yts, bermudanDates, initialVols, reversion);

	// Calibrate the GSR model to the market value of the chosen European swaptions
	LevenbergMarquardt optMethod;
	EndCriteria endCriteria(400, 100, 1.0e-8, 1.0e-8, 1.0e-8);
	for (Size i = 0; i < swaptions.size(); i++) {
		swaptions[i]->setPricingEngine(boost::make_shared<Gaussian1dJamshidianSwaptionEngine>(gsr));
	}
	cout << "Starting calibration ...\n";
	timer.start();
	gsr->calibrateVolatilitiesIterative(swaptions, optMethod, endCriteria);
	timer.stop();
	cout << "Calibration finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	// Output the calibration results
	boost::format fmter("%=11s|%=11s|%=11s|%=11s|%=11s|%=11s\n");
	cout << fmter % "Expiry" % "Tenor" % "In Vol" % "Model NPV" % "Implied" % "Diff";
	string rule(fmter.str().length(), '=');
	cout << rule << "\n";
	fmter = boost::format("%=11s|%=11s|%=11.7f|%=11.7f|%=11.7f|%=11.7f\n");
	for (Size i = 0; i < swaptions.size(); ++i) {
		Real npv = swaptions[i]->modelValue();
		Volatility implied = swaptions[i]->impliedVolatility(npv, 1e-4, 1000, 0.05, 0.50);
		Volatility diff = implied - volatilities[i];

		cout << fmter % io::short_period(expiries[i]) % io::short_period(tenors[i]) % volatilities[i] % 
			npv % implied % diff;
	}
	cout << endl;

	// Price the Bermudan
	int integrationPoints = 128;
	vector<Real> bermudanNpv(1, 0.0);
	cout << "Starting valuation ...\n";
	timer.start();
	bermudanSwaption.setPricingEngine(boost::make_shared<Gaussian1dSwaptionEngine>(gsr, integrationPoints));
	bermudanNpv[0] = bermudanSwaption.NPV();
	timer.stop();
	cout << "Valuation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";
	fmter = boost::format("%.7f\n");
	cout << "Bermudan swaption value: " << fmter % (bermudanNpv[0] / nominal);
}
