// runExample_1 implementation

#include "example_1.hpp"
#include "utilities.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/makevanillaswap.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>

#include <iostream>

using namespace QuantLib;
using std::vector;
using std::string;
using std::cout;
using std::endl;

void runExample_1() {

	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;

	// These will be the X (independent) and Y (dependent) vectors
	vector<Rate> zeroRate(1, 0.02);
	vector<Rate> swapNpv(1, 0.0);

	// Start taping with zeroRate as independent variable and set up flat zero curve
	cl::Independent(zeroRate);
	boost::shared_ptr<SimpleQuote> pZeroQuote = boost::make_shared<SimpleQuote>(zeroRate[0]);
	Handle<Quote> zeroQuote(pZeroQuote);
	Handle<YieldTermStructure> flatCurve(boost::make_shared<FlatForward>(referenceDate, zeroQuote, dayCounter));
	flatCurve->enableExtrapolation();

	// Create and price swap
	Period swapTenor(5, Years);
	boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>(flatCurve);
	Rate fixedRate = 0.03;
	Period forwardStart(0, Days);
	boost::shared_ptr<VanillaSwap> swap = MakeVanillaSwap(swapTenor, iborIndex, fixedRate, forwardStart);
	swapNpv[0] = swap->NPV();

	// Stop taping and transfer operation sequence to function f (ultimately an AD function object)
	cl::tape_function<double> f(zeroRate, swapNpv);

	// Calculate d(swapNpv) / d(zero) with forward and reverse mode
	vector<double> dZ(1, 1.0);
	double forwardDeriv = f.Forward(1, dZ)[0];
	double reverseDeriv = f.Reverse(1, dZ)[0];

	// Calculate analytically the derivative
	Real derivative = 0.0;
	const Leg& fixedLeg = swap->fixedLeg();
	for (const auto& cf : fixedLeg) {
		Real amount = cf->amount();
		Time time = dayCounter.yearFraction(referenceDate, cf->date());
		DiscountFactor discount = flatCurve->discount(time);
		derivative += amount * time * discount;
	}
	Time timeToStart = dayCounter.yearFraction(referenceDate, swap->startDate());
	Time timeToEnd = dayCounter.yearFraction(referenceDate, swap->maturityDate());
	derivative += timeToEnd * flatCurve->discount(timeToEnd) - timeToStart * flatCurve->discount(timeToStart);

	// Calculate by finite difference
	Real basisPoint = 0.0001;
	pZeroQuote->setValue(zeroRate[0] + basisPoint);
	Real oneSidedDiff = (swap->NPV() - swapNpv[0]) / basisPoint;
	pZeroQuote->setValue(zeroRate[0] - basisPoint);
	Real twoSidedDiff = (oneSidedDiff - (swap->NPV() - swapNpv[0]) / basisPoint) / 2.0;

	// Output the results
	boost::format headerFmter("%-20s|%=12s\n");
	boost::format fmter("%-20s|%=12.9f\n");

	cout << "\nCompare derivatives:\n\n";
	headerFmter % "Method" % "Derivative";
	cout << headerFmter;
	string rule(headerFmter.str().length(), '=');
	cout << rule << endl;
	cout << fmter % "Forward derivative" % forwardDeriv;
	cout << fmter % "Reverse derivative" % reverseDeriv;
	cout << fmter % "Analytic derivative" % derivative;
	cout << fmter % "One-sided FD" % oneSidedDiff;
	cout << fmter % "Two-sided FD" % twoSidedDiff;
	cout << endl;

	// Output some properties of the tape sequence
	printProperties<double>(f);
}
