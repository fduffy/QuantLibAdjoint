#pragma once

#include <ql/types.hpp>

#include <boost/format.hpp>
#include <boost/timer/timer.hpp>

#include <map>
#include <sstream>

using namespace QuantLib;

using boost::timer::cpu_times;
using boost::timer::format;
using std::cout;
using std::ostringstream;
using std::map;
using std::string;

// Only if building with AD enabled
#ifdef QL_ADJOINT
using CppAD::thread_alloc;
// Print out properties of the tape sequence in a formatted table
template <typename Base>
void printProperties(const cl::tape_function<Base>& f) {

	vector<string> properties(5);      vector<Size> numbers(5);      vector<Size> sizes(5);
	properties[0] = "f.size_op()";     numbers[0] = f.size_op();     sizes[0] = sizeof(CppAD::OpCode);
	properties[1] = "f.size_op_arg()"; numbers[1] = f.size_op_arg(); sizes[1] = sizeof(CPPAD_TAPE_ADDR_TYPE);
	properties[2] = "f.size_par()";    numbers[2] = f.size_par();    sizes[2] = sizeof(double);
	properties[3] = "f.size_text()";   numbers[3] = f.size_text();   sizes[3] = sizeof(char);
	properties[4] = "f.size_VecAD()";  numbers[4] = f.size_VecAD();  sizes[4] = sizeof(CPPAD_TAPE_ADDR_TYPE);

	Size size = 0;
	boost::format fmter("%-20s %-sB\n");
	ostringstream oss;

	cout << "Some properties of the tape sequence:\n\n";
	cout << fmter % "f.size_op_seq()" % f.size_op_seq();
	for (Size i = 0; i < properties.size(); ++i) {
		oss << numbers[i] << " x " << sizes[i] << " = " << numbers[i] * sizes[i];
		cout << fmter % properties[i] % oss.str();
		size += numbers[i] * sizes[i];
		oss.str(string());
	}
	cout << fmter % "Total" % size;

	Size thread = thread_alloc::thread_num();
	cout << fmter % "Total (in use)" % thread_alloc::inuse(thread);
}
#endif

void printTimings(const map<string, cpu_times>& timings);
