// runExample_6 implementation

#include "example_6.hpp"

#include <ql/types.hpp>
#include <ql/math/solvers1d/all.hpp>

#include <boost/make_shared.hpp>

#include <iostream>
#include <vector>
#include <iomanip>

using QuantLib::Real;
using QuantLib::Bisection;
using QuantLib::Brent;
using QuantLib::FalsePosition;
using QuantLib::FiniteDifferenceNewtonSafe;
using QuantLib::Ridder;
using QuantLib::Secant;
using QuantLib::Solver1D;

using std::vector;
using std::string;
using std::cout;
using std::setprecision;
using std::fixed;

class SquareRoot {
public:
	SquareRoot(Real y) : y_(y) {}
	Real operator()(Real x) const {
		return x * x - y_;
	}
private:
	Real y_;
};

template <class Solver>
void solve(const Solver& solver, Real value, Real accuracy, Real guess, 
	Real min, Real max, string solverName) {
	// x and y
	vector<Real> square(1, value);
	vector<Real> result(1, 0.0);

#ifdef QL_ADJOINT
	cl::Independent(square);
#endif

	SquareRoot squareRoot(square[0]);
	result[0] = solver.solve(squareRoot, accuracy, guess, min, max);

	vector<double> derivative(1, 0.0);
#ifdef QL_ADJOINT
	cl::tape_function<double> f(square, result);
	derivative = f.Forward(1, vector<double>(1, 1.0));
#endif

	cout << fixed << setprecision(9);
	cout << solverName << "," << result[0] << "," << derivative[0] << "\n";
}

void runExample_6() {
	// Set up
	Real value = 17;
	Real accuracy = 0.00000001;
	Real guess = 4.5;
	Real min = 4;
	Real max = 5;

	cout << "\nmethod,sqrt(x),sqrt'(x)\n";

	// Try solvers in turn
	solve(Bisection(), value, accuracy, guess, min, max, "Bisection");
	solve(Brent(), value, accuracy, guess, min, max, "Brent");
	solve(FiniteDifferenceNewtonSafe(), value, accuracy, guess, min, max, "FiniteDifferenceNewtonSafe");
	solve(FalsePosition(), value, accuracy, guess, min, max, "FalsePosition");
	solve(Ridder(), value, accuracy, guess, min, max, "Ridder");
	solve(Secant(), value, accuracy, guess, min, max, "Secant");

	cout << "\n";
}
