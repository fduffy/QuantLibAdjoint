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
		// May need to adjust exercise to good business day
		Date exerciseDate = coupon->accrualStartDate();
		exerciseDate = calendar.adjust(exerciseDate);
		bermudanDates.push_back(exerciseDate);
	}
	boost::shared_ptr<Exercise> bermudanExercise = boost::make_shared<BermudanExercise>(bermudanDates);
	Swaption bermudanSwaption(swap, bermudanExercise);

	// Create European swaptions that we will calibrate to
	// 1. European swaption volatilities from the market (diagonal for Bermudan swaption above)
	vector<Period> expiries{ 5 * Years, 10 * Years, 15 * Years, 20 * Years };
	vector<Period> tenors{ 20 * Years, 15 * Years, 10 * Years, 5 * Years };
	vector<Volatility> volatilities{ 0.18, 0.16, 0.14, 0.12 };
	Size numVols = expiries.size();

	vector<Real> helperNpv(numVols, 0.0);
	vector<double> dhelperNpvdvol(numVols * numVols, 0.0);
#ifdef QL_ADJOINT
	// Start taping with input market volatilities for dHelper / dvol
	cl::Independent(volatilities);
#endif

	// 2. Create the calibration helpers
	vector<boost::shared_ptr<CalibrationHelper>> swaptions(numVols);
	vector<boost::shared_ptr<SimpleQuote>> volQuotes(numVols);
	for (Size i = 0; i < numVols; ++i) {
		volQuotes[i] = boost::make_shared<SimpleQuote>(volatilities[i]);
		swaptions[i] = boost::make_shared<SwaptionHelper>(expiries[i], tenors[i], Handle<Quote>(volQuotes[i]),
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
		helperNpv[i] = swaptions[i]->marketValue();
	}

	cout << "Starting dHelperNpv / dvol evaluation ...\n";
	timer.start();
#ifdef QL_ADJOINT
	// Stop taping
	cl::tape_function<double> fnHelperNpv(volatilities, helperNpv);

	// Calculate the Jacobian dHelperNpv / dvol for each input volatility
	vector<double> v_0(numVols);
	for (Size i = 0; i < numVols; ++i) {
		v_0[i] = Value(volatilities[i].value());
	}
	dhelperNpvdvol = fnHelperNpv.Jacobian(v_0);
#endif
	timer.stop();
	Size idx = 0;
	boost::format fmter = boost::format(" %+.7f ");
	cout << "  dHelperNpv / dvol:\n";
	for (Size i = 0; i < numVols; ++i) {
		cout << "    |";
		for (Size j = 0; j < numVols; ++j) {
			idx = i * numVols + j;
			cout << fmter % dhelperNpvdvol[idx];
		}
		cout << "|\n";
	}
	cout << "dHelperNpv / dvol evaluation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	cout << "Starting calibration ...\n\n";
	timer.start();
	gsr->calibrateVolatilitiesIterative(swaptions, optMethod, endCriteria);
	timer.stop();

	// Output the calibration results
	fmter = boost::format("  %=11s|%=11s|%=11s|%=11s|%=11s|%=11s\n");
	cout << fmter % "Expiry" % "Tenor" % "In Vol" % "Model NPV" % "Implied" % "Diff";
	string rule(fmter.str().length() - 2, '=');
	cout << "  " << rule << "\n";
	fmter = boost::format("  %=11s|%=11s|%=11.7f|%=11.7f|%=11.7f|%=11.7f\n");
	for (Size i = 0; i < swaptions.size(); ++i) {
		Real npv = swaptions[i]->modelValue();
		Volatility implied = swaptions[i]->impliedVolatility(npv, 1e-4, 1000, 0.05, 0.50);
		Volatility diff = implied - volatilities[i];

		cout << fmter % io::short_period(expiries[i]) % io::short_period(tenors[i]) % volatilities[i] % 
			npv % implied % diff;
	}
	cout << endl;
	cout << "Calibration finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	// Reset the GSR with the result of the calibration i.e. calibrated volatilites
	// Do this so we can tape wrt the calibrated volatilites
	Array tempVols = gsr->volatility();
	vector<Real> calibratedVols(tempVols.begin(), tempVols.end());
	Size numSigmas = calibratedVols.size();
#ifdef QL_ADJOINT
	// Start taping with calibrated volatilities for dBermudan / dsigma
	cl::Independent(calibratedVols);
#endif
	gsr = boost::make_shared<Gsr>(yts, bermudanDates, calibratedVols, reversion);

	// Price the Bermudan
	int integrationPoints = 128;
	vector<Real> bermudanNpv(1, 0.0);
	cout << "Starting valuation ...\n";
	timer.start();
	bermudanSwaption.setPricingEngine(boost::make_shared<Gaussian1dSwaptionEngine>(gsr, integrationPoints));
	bermudanNpv[0] = bermudanSwaption.NPV();
	timer.stop();
	fmter = boost::format("%.7f");
	cout << "  Bermudan swaption value: " << fmter % (bermudanNpv[0] / nominal) << "\n";
	cout << "Valuation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	// Calculate the Jacobian of bermudan wrt calibrated volatilities i.e. sigmas
	vector<double> dBermudandsigma(numSigmas, 0.0);
	cout << "Starting dBermudan / dsigma evaluation ...\n";
	timer.start();
#ifdef QL_ADJOINT
	// Stop taping
	cl::tape_function<double> fnBermudan(calibratedVols, bermudanNpv);

	// Calculate the Jacobian dBermudan / dsigma
	vector<double> sigma_0(numSigmas);
	for (Size i = 0; i < numSigmas; ++i) {
		sigma_0[i] = Value(calibratedVols[i].value());
	}
	dBermudandsigma = fnBermudan.Jacobian(sigma_0);
#endif
	timer.stop();
	fmter = boost::format(" %.7f ");
	cout << "  dBermudan / dsigma: [";
	for (Size i = 0; i < numSigmas; ++i)
		cout << fmter % dBermudandsigma[i];
	cout << "]\n";
	cout << "dBermudan / dsigma evaluation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	// We also need the inverse of dHelper / dsigma
	vector<Real> helperModelNpv(numVols, 0.0);
#ifdef QL_ADJOINT
	// Start taping with calibrated volatilities for dHelper / dsigma
	cl::Independent(calibratedVols);
#endif
	gsr = boost::make_shared<Gsr>(yts, bermudanDates, calibratedVols, reversion);

	// Calculate the model value of each helper
	for (Size i = 0; i < swaptions.size(); i++) {
		swaptions[i]->setPricingEngine(boost::make_shared<Gaussian1dJamshidianSwaptionEngine>(gsr));
		helperModelNpv[i] = swaptions[i]->modelValue();
	}

	// Calculate the Jacobian of helper wrt sigmas
	vector<double> dHelperdsigma(numVols * numSigmas, 0.0);
	cout << "Starting dHelper / dsigma evaluation ...\n";
	timer.start();
#ifdef QL_ADJOINT
	// Stop taping
	cl::tape_function<double> fnHelperModelNpv(calibratedVols, helperModelNpv);
	for (Size i = 0; i < numSigmas; ++i) {
		sigma_0[i] = Value(calibratedVols[i].value());
	}
	dHelperdsigma = fnHelperModelNpv.Jacobian(sigma_0);
#endif
	timer.stop();
	fmter = boost::format(" %+.7f ");
	cout << "  dHelper / dsigma:\n";
	for (Size i = 0; i < numVols; ++i) {
		cout << "    |";
		for (Size j = 0; j < numSigmas; ++j) {
			idx = i * numSigmas + j;
			cout << fmter % dHelperdsigma[idx];
		}
		cout << "|\n";
	}
	cout << "dHelper / dsigma evaluation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

	// Calculate the vegas by one-sided finite difference
	Real delta = 0.0001;
	vector<Real> oneSidedDiffs(numVols, 0.0);
	bermudanSwaption.setPricingEngine(boost::make_shared<Gaussian1dSwaptionEngine>(gsr, integrationPoints));
	cout << "Starting 1-sided FD evaluation ...\n";
	timer.start();
	for (Size i = 0; i < numVols; ++i) {
		// Shift input volatility up by delta
		volQuotes[i]->setValue(volatilities[i] + delta);
		gsr->calibrateVolatilitiesIterative(swaptions, optMethod, endCriteria);
		oneSidedDiffs[i] = (bermudanSwaption.NPV() - bermudanNpv[0]) / delta;
		// Reset to original volatility
		volQuotes[i]->setValue(volatilities[i]);
	}
	timer.stop();
	fmter = boost::format(" %.7f ");
	cout << "  1-sided FD: [";
	for (Size i = 0; i < numVols; ++i)
		cout << fmter % oneSidedDiffs[i];
	cout << "]\n";
	cout << "1-sided FD evaluation finished, time taken: " << format(timer.elapsed(), 6, "%w") << "\n\n";

}
