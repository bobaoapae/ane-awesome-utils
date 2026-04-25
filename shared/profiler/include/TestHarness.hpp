// Minimalist header-only test harness. No external dependency, no macros
// that conflict with Windows. Just enough to assert things in unit tests.
//
// Usage:
//   #include "TestHarness.hpp"
//   TEST("name of test") {
//     EXPECT(x == 42);
//     EXPECT_EQ(a, b);
//   }
//   int main() { return ane::profiler::test::run_all(); }

#ifndef ANE_PROFILER_TEST_HARNESS_HPP
#define ANE_PROFILER_TEST_HARNESS_HPP

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace ane::profiler::test {

struct Case {
    std::string name;
    std::function<void()> fn;
    const char* file;
    int line;
};

inline std::vector<Case>& cases() {
    static std::vector<Case> v;
    return v;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn, const char* file, int line) {
        cases().push_back({name, std::move(fn), file, line});
    }
};

struct AssertionFailed : std::exception {
    std::string message;
    AssertionFailed(std::string msg) : message(std::move(msg)) {}
    const char* what() const noexcept override { return message.c_str(); }
};

inline int run_all() {
    int pass = 0, fail = 0;
    for (const auto& c : cases()) {
        std::fprintf(stdout, "RUN   %s\n", c.name.c_str());
        std::fflush(stdout);
        try {
            c.fn();
            std::fprintf(stdout, "  OK  %s\n", c.name.c_str());
            std::fflush(stdout);
            ++pass;
        } catch (const AssertionFailed& e) {
            std::fprintf(stdout, "FAIL  %s: %s\n", c.name.c_str(), e.what());
            std::fflush(stdout);
            ++fail;
        } catch (const std::exception& e) {
            std::fprintf(stdout, "FAIL  %s: unexpected exception: %s\n", c.name.c_str(), e.what());
            std::fflush(stdout);
            ++fail;
        } catch (...) {
            std::fprintf(stdout, "FAIL  %s: unknown exception\n", c.name.c_str());
            std::fflush(stdout);
            ++fail;
        }
    }
    std::fprintf(stdout, "\n==== %d passed, %d failed ====\n", pass, fail);
    std::fflush(stdout);
    return fail;
}

} // namespace ane::profiler::test

// TEST("name") { body }
#define ANE_PP_CAT2(a, b) a##b
#define ANE_PP_CAT(a, b) ANE_PP_CAT2(a, b)
#define TEST(NAME)                                                                 \
    static void ANE_PP_CAT(ane_test_fn_, __LINE__)();                              \
    static const ::ane::profiler::test::Registrar ANE_PP_CAT(ane_test_reg_, __LINE__){ \
        NAME, &ANE_PP_CAT(ane_test_fn_, __LINE__), __FILE__, __LINE__};            \
    static void ANE_PP_CAT(ane_test_fn_, __LINE__)()

#define EXPECT(COND) do {                                                          \
    if (!(COND)) {                                                                 \
        std::ostringstream _oss;                                                   \
        _oss << __FILE__ << ":" << __LINE__ << "  EXPECT(" #COND ") failed";       \
        throw ::ane::profiler::test::AssertionFailed(_oss.str());                  \
    }                                                                              \
} while (0)

#define EXPECT_EQ(A, B) do {                                                       \
    const auto _a = (A);                                                           \
    const auto _b = (B);                                                           \
    if (!(_a == _b)) {                                                             \
        std::ostringstream _oss;                                                   \
        _oss << __FILE__ << ":" << __LINE__                                        \
             << "  EXPECT_EQ(" #A ", " #B ") failed  lhs=" << _a << " rhs=" << _b; \
        throw ::ane::profiler::test::AssertionFailed(_oss.str());                  \
    }                                                                              \
} while (0)

#endif // ANE_PROFILER_TEST_HARNESS_HPP
