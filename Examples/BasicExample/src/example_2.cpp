// runExample_2 implementation

#include "example_2.hpp"
#include "utilities.hpp"

#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/makevanillaswap.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>

#include <iostream>

using namespace QuantLib;
using std::vector;
using std::string;
using std::cout;
using std::endl;

void runExample_2() {

	Date referenceDate(3, Aug, 2016);
	Settings::instance().evaluationDate() = referenceDate;
	Actual365Fixed dayCounter;

	// These will be the X (independent) and Y (dependent) vectors. f: R^5 -> R.
	vector<Rate> zeroRates{ 0.02, 0.025, 0.0275, 0.03, 0.035 };
	vector<Date> zeroDates{ referenceDate, Date(3, Aug, 2018), Date(3, Aug, 2019), 
		Date(3, Aug, 2021), Date(3, Aug, 2026) };
	vector<Rate> swapNpv(1, 0.0);

#ifdef QL_ADJOINT
	// Start taping with zeroRates as independent variable
	cl::Independent(zeroRates);
#endif

	// Set up linearly interpolated zero curve
	RelinkableHandle<YieldTermStructure> zeroCurve(boost::make_shared<ZeroCurve>(zeroDates, zeroRates, dayCounter));

	// Create and price swap
	Period swapTenor(5, Years);
	boost::shared_ptr<IborIndex> iborIndex = boost::make_shared<Euribor6M>(zeroCurve);
	Rate fixedRate = 0.0325;
	Period forwardStart(0, Days);
	boost::shared_ptr<VanillaSwap> swap = MakeVanillaSwap(swapTenor, iborIndex, fixedRate, forwardStart);
	swapNpv[0] = swap->NPV();

#ifdef QL_ADJOINT
	// Stop taping and transfer operation sequence to function f (ultimately an AD function object)
	cl::tape_function<double> f(zeroRates, swapNpv);
#endif

	// Calculate d(swapNpv) / d(z_i) for i = 1, ..., nZeros
	// ... with forward mode
	Size nZeros = zeroRates.size();
	vector<double> forwardDerivs(nZeros, 0.0);
	vector<double> reverseDerivs(nZeros, 0.0);
#ifdef QL_ADJOINT
	vector<double> dZ(nZeros, 0.0);
	for (Size i = 0; i < nZeros; ++i) {
		dZ[i] = 1.0;
		forwardDerivs[i] = f.Forward(1, dZ)[0];
		dZ[i] = 0.0;

	}
	// ... with reverse mode
	reverseDerivs = f.Reverse(1, vector<double>(1, 1.0));
#endif

	// Calculate the derivatives by finite difference i.e. bump each zero in turn
	Real basisPoint = 0.0001;
	vector<Real> oneSidedDiffs(nZeros, 0.0);
	vector<Real> twoSidedDiffs(nZeros, 0.0);
	for (Size i = 0; i < nZeros; ++i) {
		// Up 1 bp
		zeroRates[i] += basisPoint;
		zeroCurve.linkTo(boost::make_shared<ZeroCurve>(zeroDates, zeroRates, dayCounter));
		oneSidedDiffs[i] = (swap->NPV() - swapNpv[0]) / basisPoint;
		// Down 1 bp
		zeroRates[i] -= 2 * basisPoint;
		zeroCurve.linkTo(boost::make_shared<ZeroCurve>(zeroDates, zeroRates, dayCounter));
		twoSidedDiffs[i] = (oneSidedDiffs[i] - (swap->NPV() - swapNpv[0]) / basisPoint) / 2.0;
		// Reset to original curve
		zeroRates[i] += basisPoint;
		zeroCurve.linkTo(boost::make_shared<ZeroCurve>(zeroDates, zeroRates, dayCounter));
	}

	// Output the results
	boost::format headerFmter("%-10s|%=10s|%=10s|%=10s|%=10s");
	boost::format fmter("z_%-8s|%=10.8f|%=10.8f|%=10.8f|%=10.8f");
	
	cout << endl;
	cout << "Compare derivatives:" << endl;
	cout << endl;
	headerFmter % "Zero Rate" % "Forward" % "Reverse" % "One FD" % "Two FD";
	cout << headerFmter;
	string rule(headerFmter.str().length(), '=');
	cout << endl << rule << endl;
	for (Size i = 0; i < nZeros; ++i) {
		cout << fmter % (i + 1) % forwardDerivs[i] % reverseDerivs[i] % oneSidedDiffs[i] % twoSidedDiffs[i] << endl;
	}
	cout << endl;

#ifdef QL_ADJOINT
	// Output some properties of the tape sequence
	printProperties<double>(f);
#endif
}