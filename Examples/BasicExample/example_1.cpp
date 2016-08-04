/* runExample_1 implementation
*/

#include "example_1.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/makevanillaswap.hpp>
#include <ql/termstructures/yield/flatforward.hpp>

#include <boost/make_shared.hpp>

#include <iostream>

using namespace QuantLib;
using std::vector;
using std::cout;
using std::endl;
using std::ios;

void runExample_1() {

	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;

	// Example 1

	// These will be the X (independent) and Y (dependent) vectors
	vector<Rate> zeroRate(1, 0.02);
	vector<Rate> swapNpv(1, 0.0);

	// Start taping with zeroRate as independent variable and set up flat zero curve
	cl::Independent(zeroRate);
	Handle<YieldTermStructure> flatCurve(boost::make_shared<FlatForward>(referenceDate, zeroRate[0], dayCounter));
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

	// Output the results
	cout.precision(9);
	cout.setf(ios::fixed, ios::floatfield);
	cout << "Forward derivative:  " << forwardDeriv << endl;
	cout << "Reverse derivative:  " << reverseDeriv << endl;
	cout << "Analytic derivative: " << derivative << endl;
}
