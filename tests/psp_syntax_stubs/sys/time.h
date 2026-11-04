#pragma once
// Syntax checks against PSPSDK declarations on Windows, not a runtime shim.
struct timeval {long tv_sec,tv_usec;};
struct timezone {int tz_minuteswest,tz_dsttime;};
