/* Basic example showing the usage of QuantLibAdjoint
*/

#include "example_1.hpp"
#include "example_2.hpp"
#include "example_3.hpp"
#include "example_4.hpp"
#include "example_5.hpp"
#include "example_6.hpp"
#include "example_7.hpp"
#include "example_8.hpp"

#include <ql/errors.hpp>
#include <ql/types.hpp>

#include <boost/lexical_cast.hpp>
#ifdef BOOST_MSVC
#define BOOST_LIB_NAME boost_timer
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_chrono
#include <boost/config/auto_link.hpp>
#define BOOST_LIB_NAME boost_system
#include <boost/config/auto_link.hpp>
#undef BOOST_LIB_NAME
#endif

#include <iostream>

using QuantLib::Size;
using QuantLib::Error;
using std::cout;
using std::endl;

int main(int argc, char* argv[]) {
	try {
		// Check number of arguments
		QL_REQUIRE(argc < 3, "Too many command line arguments supplied!");

		Size exampleIndex = 1;
		if (argc == 2) {
			// If one command line parameter passed, try to convert it to positive integer
			QL_REQUIRE(*argv[1] != '-', "Must provide positive integers");
			QL_REQUIRE(boost::conversion::try_lexical_convert(argv[1], exampleIndex),
				"Cannot convert the command line parameter " << argv[1] << " to an unsigned integer");
		}

		// Choose example
		switch (exampleIndex) {
		case 1:
			runExample_1();
			break;
		case 2:
			runExample_2();
			break;
		case 3:
			runExample_3();
			break;
		case 4:
			runExample_4();
			break;
		case 5:
			runExample_5();
			break;
		case 6:
			runExample_6();
			break;
		case 7:
			runExample_7();
			break;
		case 8:
			runExample_8();
			break;
		default:
			QL_FAIL("The example with example index " << exampleIndex << " does not exist.");
			break;
		}
	} catch (Error& e) {
		cout << "QuantLib Error: " << e.what() << endl;
	}

	return 0;
}
