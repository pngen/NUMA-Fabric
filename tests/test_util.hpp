#pragma once
// ============================================================================
// NUMA Fabric test helpers. Minimal dependency-free CHECK framework. Each test
// is an ordinary executable that returns 0 on success / non-zero on failure.
// No test-timeout mechanism is used anywhere.
// ============================================================================

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>

namespace nftest {
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
} // namespace nftest

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++nftest::checks();                                                             \
        if (!(cond)) {                                                                  \
            ++nftest::failures();                                                       \
            std::printf("CHECK FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                               \
    } while (0)

#define CHECK_MSG(cond, msg)                                                            \
    do {                                                                                \
        ++nftest::checks();                                                             \
        if (!(cond)) {                                                                  \
            ++nftest::failures();                                                       \
            std::printf("CHECK FAILED at %s:%d: %s  (%s)\n", __FILE__, __LINE__, #cond, \
                        std::string(msg).c_str());                                      \
        }                                                                               \
    } while (0)

#define CHECK_THROWS(expr)                                                              \
    do {                                                                                \
        ++nftest::checks();                                                             \
        bool thrown_ = false;                                                           \
        try { (void)(expr); } catch (...) { thrown_ = true; }                           \
        if (!thrown_) {                                                                 \
            ++nftest::failures();                                                       \
            std::printf("CHECK_THROWS FAILED at %s:%d: %s\n", __FILE__, __LINE__, #expr);\
        }                                                                               \
    } while (0)

#define RUN_TEST(name)                                                                  \
    do {                                                                                \
        std::printf("[test] %s\n", #name);                                              \
        std::fflush(stdout);                                                            \
        try { name(); }                                                                 \
        catch (const std::exception& e) {                                               \
            ++nftest::failures();                                                       \
            std::printf("  EXCEPTION in %s: %s\n", #name, e.what());                    \
        } catch (...) {                                                                 \
            ++nftest::failures();                                                       \
            std::printf("  UNKNOWN EXCEPTION in %s\n", #name);                          \
        }                                                                               \
        std::fflush(stdout);                                                            \
    } while (0)

#define TEST_MAIN()                                                                     \
    int main() {                                                                        \
        try { run_all(); }                                                              \
        catch (const std::exception& e) {                                               \
            ++nftest::failures();                                                       \
            std::printf("top-level EXCEPTION: %s\n", e.what());                        \
        }                                                                               \
        std::printf("checks=%d failures=%d\n", nftest::checks(), nftest::failures());   \
        return nftest::failures() == 0 ? 0 : 1;                                         \
    }
