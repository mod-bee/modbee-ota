#pragma once

// Included last by each library source file, after every other include.
//
// The library throws nothing and catches nothing, so on a board its own
// functions need no exception tables or unwind code. Placed after the standard
// headers so they keep their own settings. The host build is unchanged.
#include "OtaConfig.h"

#if MODBEE_OTA_LEAN && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("no-exceptions")
#endif
