#include "utilities.hpp"

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