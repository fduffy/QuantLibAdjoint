#include "utilities.hpp"

#include <ql/instruments/makevanillaswap.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>

void printTimings(const map<string, cpu_times>& timings) {
	cout << "Timings in seconds:\n\n";

	// Print out the header
	boost::format fmter("%-16s|%=10s|%=10s|%=10s\n");
	cout << fmter % "Task" % "Wall" % "User" % "System";

	string rule(fmter.str().length(), '=');
	cout << rule << "\n";

	// Print out the table
	for (auto& val : timings) {
		const cpu_times& t = val.second;
		cout << fmter % val.first % format(t, 6, "%w") % format(t, 6, "%u") % format(t, 6, "%s");
	}
	cout << "\n";
}

vector<boost::shared_ptr<VanillaSwap>> makePortfolio(Size nSwaps, const Period& swapTenor,
	const boost::shared_ptr<IborIndex>& iborIndex) {
	
	// Create a portfolio of swaps with random fixed rates in [1.5%, 4.5%]
	MersenneTwisterUniformRng rng(3);
	vector<boost::shared_ptr<VanillaSwap>> portfolio(nSwaps);
	for (Size i = 0; i < nSwaps; i++) {
		Real fixedRate = 0.015 + rng.nextReal() * 0.03;
		portfolio[i] = MakeVanillaSwap(swapTenor, iborIndex, fixedRate, 0*Days);
	}
	return portfolio;
}
