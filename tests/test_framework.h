#pragma once
// =============================================================================
// tests/test_framework.h — Minimal self-contained assertion framework
// =============================================================================
// Deliberately hand-rolled rather than vendored (doctest/Catch2): the whole
// framework is ~200 lines, has zero network/vcpkg/FetchContent dependencies,
// and compiles in well under a second. There is nothing here a third-party
// framework would do better for this project's needs.
//
// Usage:
//     #include "test_framework.h"
//
//     TEST_CASE(MyThing_DoesTheRightThing)
//     {
//         CHECK(1 + 1 == 2);
//         CHECK_EQ(Add(2, 2), 4);
//         CHECK_APPROX(std::sqrt(2.0f), 1.41421356f);
//     }
//
// Test cases self-register via a file-static AutoReg object, so adding a new
// .cpp to the TheDawningTests target is all that is required.
//
// -----------------------------------------------------------------------------
// CHECK_KNOWN_FAILING
// -----------------------------------------------------------------------------
// Used to encode a defect that is *identified and documented but not yet fixed*.
// The expression states the CORRECT behaviour. It is expected to evaluate false
// today. Such a check:
//   - never fails the suite (exit code stays 0),
//   - prints a loud KNOWN FAILURE notice with the defect id,
//   - prints an even louder "APPEARS FIXED" notice if it ever starts passing,
//     which is the signal to promote it to a plain CHECK.
//
// This lets the suite carry a regression test for a defect owned by someone
// else without holding the build hostage.
// =============================================================================

#include <cstdio>
#include <cmath>
#include <vector>
#include <type_traits>

namespace testing
{

using TestFn = void (*)();

struct TestCase
{
    const char* name = nullptr;
    TestFn      fn   = nullptr;
};

inline std::vector<TestCase>& Cases()
{
    static std::vector<TestCase> s_cases;
    return s_cases;
}

struct Counters
{
    int checks              = 0;   // total checks evaluated
    int failures            = 0;   // real failures — these fail the suite
    int currentTestFailures = 0;   // failures within the running test case
    int knownStillBroken    = 0;   // documented defect still reproduces
    int knownNowFixed       = 0;   // documented defect no longer reproduces
};

inline Counters& Stats()
{
    static Counters s_stats;
    return s_stats;
}

struct AutoReg
{
    AutoReg(const char* name, TestFn fn) { Cases().push_back(TestCase{ name, fn }); }
};

// -----------------------------------------------------------------------------
// Value printing — numeric values print their value, everything else is elided.
// -----------------------------------------------------------------------------
template<typename T>
inline void PrintValue(const T& v)
{
    if constexpr (std::is_arithmetic_v<T>)
        std::printf("%.9g", static_cast<double>(v));
    else
        std::printf("(non-printable)");
}

inline void RecordFailure()
{
    Stats().failures++;
    Stats().currentTestFailures++;
}

// -----------------------------------------------------------------------------
// Check implementations
// -----------------------------------------------------------------------------
inline bool ReportCheck(bool ok, const char* expr, const char* file, int line)
{
    Stats().checks++;
    if (!ok)
    {
        RecordFailure();
        std::printf("    FAIL  %s(%d)\n", file, line);
        std::printf("          CHECK( %s )\n", expr);
    }
    return ok;
}

template<typename A, typename B>
inline bool ReportCheckEq(const A& a, const B& b, const char* exprA, const char* exprB,
                          const char* file, int line)
{
    Stats().checks++;
    const bool ok = (a == b);
    if (!ok)
    {
        RecordFailure();
        std::printf("    FAIL  %s(%d)\n", file, line);
        std::printf("          CHECK_EQ( %s , %s )\n", exprA, exprB);
        std::printf("            lhs = "); PrintValue(a); std::printf("\n");
        std::printf("            rhs = "); PrintValue(b); std::printf("\n");
    }
    return ok;
}

inline bool ReportCheckApprox(double a, double b, double eps,
                              const char* exprA, const char* exprB,
                              const char* file, int line)
{
    Stats().checks++;
    const bool ok = std::fabs(a - b) <= eps;
    if (!ok)
    {
        RecordFailure();
        std::printf("    FAIL  %s(%d)\n", file, line);
        std::printf("          CHECK_APPROX( %s , %s )\n", exprA, exprB);
        std::printf("            lhs = %.9g\n", a);
        std::printf("            rhs = %.9g\n", b);
        std::printf("            |diff| = %.9g  (tolerance %.9g)\n", std::fabs(a - b), eps);
    }
    return ok;
}

// `expr` states the CORRECT behaviour and is expected to be false today.
// Never contributes to the suite's exit code — see the header comment.
inline void ReportKnownFailing(bool ok, const char* expr, const char* defectId,
                               const char* file, int line)
{
    Stats().checks++;
    if (!ok)
    {
        Stats().knownStillBroken++;
        std::printf("    KNOWN FAILURE [%s]  %s(%d)\n", defectId, file, line);
        std::printf("          correct behaviour would be: %s\n", expr);
    }
    else
    {
        Stats().knownNowFixed++;
        std::printf("    *********************************************************\n");
        std::printf("    *** DEFECT %s APPEARS FIXED — ACTION REQUIRED         \n", defectId);
        std::printf("    ***   %s(%d)\n", file, line);
        std::printf("    ***   promote CHECK_KNOWN_FAILING -> CHECK:\n");
        std::printf("    ***   %s\n", expr);
        std::printf("    *********************************************************\n");
    }
}

// -----------------------------------------------------------------------------
// Runner
// -----------------------------------------------------------------------------
inline int RunAll()
{
    const int total = static_cast<int>(Cases().size());
    std::printf("=================================================================\n");
    std::printf(" The Dawning V3 — unit tests (%d case%s)\n", total, total == 1 ? "" : "s");
    std::printf(" CPU only: no D3D12, no device, no GPU required.\n");
    std::printf("=================================================================\n\n");

    int passed = 0;
    int failed = 0;

    for (const TestCase& tc : Cases())
    {
        Stats().currentTestFailures = 0;
        std::printf("[ RUN      ] %s\n", tc.name);
        tc.fn();

        if (Stats().currentTestFailures == 0)
        {
            std::printf("[       OK ] %s\n", tc.name);
            passed++;
        }
        else
        {
            std::printf("[  FAILED  ] %s (%d failed check%s)\n",
                        tc.name,
                        Stats().currentTestFailures,
                        Stats().currentTestFailures == 1 ? "" : "s");
            failed++;
        }
    }

    const Counters& s = Stats();
    std::printf("\n=================================================================\n");
    std::printf(" cases  : %d passed, %d failed, %d total\n", passed, failed, total);
    std::printf(" checks : %d evaluated, %d failed\n", s.checks, s.failures);

    if (s.knownStillBroken > 0)
    {
        std::printf("\n -----------------------------------------------------------\n");
        std::printf("  NOTICE: %d known-failing check%s did NOT pass, as expected.\n",
                    s.knownStillBroken, s.knownStillBroken == 1 ? "" : "s");
        std::printf("  These encode defects that are documented but not yet fixed.\n");
        std::printf("  They do not fail the suite. See docs/ANALYSIS.md section 4.\n");
        std::printf(" -----------------------------------------------------------\n");
    }
    if (s.knownNowFixed > 0)
    {
        std::printf("\n  ACTION REQUIRED: %d known-failing check%s now PASSING.\n",
                    s.knownNowFixed, s.knownNowFixed == 1 ? " is" : "s are");
        std::printf("  The underlying defect appears fixed — promote the check(s)\n");
        std::printf("  from CHECK_KNOWN_FAILING to CHECK so they guard the fix.\n");
    }

    std::printf("\n %s\n", failed == 0 ? "RESULT: PASS" : "RESULT: FAIL");
    std::printf("=================================================================\n");

    return failed == 0 ? 0 : 1;
}

} // namespace testing

// =============================================================================
// Macros
// =============================================================================
#define TD_TEST_CONCAT_(a, b) a##b
#define TD_TEST_CONCAT(a, b)  TD_TEST_CONCAT_(a, b)

#define TEST_CASE(name)                                                            \
    static void name();                                                            \
    static ::testing::AutoReg TD_TEST_CONCAT(s_autoReg_, name)(#name, &name);      \
    static void name()

#define CHECK(expr) \
    ::testing::ReportCheck(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

#define CHECK_FALSE(expr) \
    ::testing::ReportCheck(!static_cast<bool>(expr), "!(" #expr ")", __FILE__, __LINE__)

#define CHECK_EQ(a, b) \
    ::testing::ReportCheckEq((a), (b), #a, #b, __FILE__, __LINE__)

// Default tolerance is loose enough for accumulated float32 error in a
// 4x4 matrix product, tight enough to catch a wrong axis or a sign flip.
#define CHECK_APPROX(a, b)                                                         \
    ::testing::ReportCheckApprox(static_cast<double>(a), static_cast<double>(b),    \
                                 1e-5, #a, #b, __FILE__, __LINE__)

#define CHECK_APPROX_EPS(a, b, eps)                                                \
    ::testing::ReportCheckApprox(static_cast<double>(a), static_cast<double>(b),    \
                                 static_cast<double>(eps), #a, #b, __FILE__, __LINE__)

// See the header comment. `expr` is the CORRECT behaviour, expected false today.
#define CHECK_KNOWN_FAILING(expr, defectId)                                        \
    ::testing::ReportKnownFailing(static_cast<bool>(expr), #expr, defectId,         \
                                  __FILE__, __LINE__)
