#pragma once

// A ~100-line test harness rather than a vendored framework: the whole suite must stay linkable
// against VAE-Core alone, and nothing here is worth a submodule.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace test {

    struct Case {
        std::string suite;
        std::string name;
        std::function<void()> fn;
    };

    inline std::vector<Case>& Registry() { static std::vector<Case> cases; return cases; }
    inline int& Failures() { static int n = 0; return n; }
    inline int& Checks()   { static int n = 0; return n; }
    inline bool& CurrentFailed() { static bool f = false; return f; }

    struct Registrar {
        Registrar(const char* suite, const char* name, std::function<void()> fn) {
            Registry().push_back({ suite, name, std::move(fn) });
        }
    };

    inline void Fail(const char* file, int line, const std::string& what) {
        ++Failures();
        CurrentFailed() = true;
        std::printf("      FAIL %s:%d — %s\n", file, line, what.c_str());
    }

    inline bool NearEqual(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

    inline int RunAll(const char* filter = nullptr) {
        std::string lastSuite;
        int passed = 0;
        for (auto& c : Registry()) {
            if (filter && c.suite.find(filter) == std::string::npos
                       && c.name.find(filter) == std::string::npos) continue;
            if (c.suite != lastSuite) { std::printf("  %s\n", c.suite.c_str()); lastSuite = c.suite; }
            CurrentFailed() = false;
            c.fn();
            if (CurrentFailed()) std::printf("    ✗ %s\n", c.name.c_str());
            else { std::printf("    ✓ %s\n", c.name.c_str()); ++passed; }
        }
        std::printf("\n%d passed, %d failed (%d checks)\n", passed, Failures(), Checks());
        return Failures() == 0 ? 0 : 1;
    }

}

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)

#define TEST(suite, name)                                                                      \
    static void TEST_CAT(test_fn_, __LINE__)();                                                 \
    static ::test::Registrar TEST_CAT(test_reg_, __LINE__){ #suite, #name, TEST_CAT(test_fn_, __LINE__) }; \
    static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(cond)                                                                            \
    do { ++::test::Checks();                                                                    \
         if (!(cond)) ::test::Fail(__FILE__, __LINE__, "CHECK(" #cond ")"); } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do { ++::test::Checks();                                                                    \
         auto&& _a = (a); auto&& _b = (b);                                                      \
         if (!(_a == _b)) ::test::Fail(__FILE__, __LINE__,                                       \
             std::string("CHECK_EQ(" #a ", " #b ")")); } while (0)

// For a check whose failure needs to carry something with it — a compiler's diagnostics, a path.
#define CHECK_MESSAGE(cond, message)                                                           \
    do { ++::test::Checks();                                                                    \
         if (!(cond)) ::test::Fail(__FILE__, __LINE__,                                          \
             std::string("CHECK(" #cond ") — ") + (message)); } while (0)

#define CHECK_NEAR(a, b)                                                                       \
    do { ++::test::Checks();                                                                    \
         if (!::test::NearEqual((a), (b))) ::test::Fail(__FILE__, __LINE__,                      \
             std::string("CHECK_NEAR(" #a ", " #b ") — got ") + std::to_string((double)(a))      \
             + " vs " + std::to_string((double)(b))); } while (0)

// Layout answers are exact to the solver and approximate to a person: a pane that is 70% of a
// 760px box lands on 532.0, not on whatever 0.7 is in binary. A tolerance says which one is meant.
#define CHECK_NEAR_EPS(a, b, eps)                                                                \
    do { ++::test::Checks();                                                                    \
         if (!::test::NearEqual((a), (b), (eps))) ::test::Fail(__FILE__, __LINE__,                \
             std::string("CHECK_NEAR_EPS(" #a ", " #b ") — got ") + std::to_string((double)(a))  \
             + " vs " + std::to_string((double)(b))); } while (0)
