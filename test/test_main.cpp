// =============================================================================
// test_main.cpp — doctest entry point for JT-8000 v2 host tests
// =============================================================================
// One translation unit owns the doctest implementation; the actual tests
// live in test_curves.cpp and test_parameter_store.cpp.
// Build & run:  make test   (see Makefile — same strict flags as firmware).
// =============================================================================
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
