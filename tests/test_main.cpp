// =============================================================================
// tests/test_main.cpp — Entry point for TheDawningTests
// =============================================================================
// Returns 0 when every non-known-failing check passed, nonzero otherwise.
// ctest keys off this exit code.
// =============================================================================

#include "test_framework.h"

int main()
{
    return ::testing::RunAll();
}
