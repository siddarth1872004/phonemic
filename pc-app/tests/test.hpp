// A tiny self-registering test harness (no external test framework needed).
#pragma once

#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <string>
#include <vector>

namespace t {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}
struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <typename A, typename B>
void eq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::ostringstream o;
        o << file << ":" << line << ": expected " << ea << " == " << eb;
        if constexpr (std::is_arithmetic_v<A> && std::is_arithmetic_v<B>) o << " (" << +a << " vs " << +b << ")";
        throw Failure(o.str());
    }
}

}  // namespace t

#define T_CAT2(a, b) a##b
#define T_CAT(a, b) T_CAT2(a, b)
#define TEST(name)                                                   \
    static void name();                                              \
    static t::Reg T_CAT(reg_, name)(#name, name);                    \
    static void name()
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) throw t::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": CHECK(" #cond ")"); \
    } while (0)
#define CHECK_EQ(a, b) t::eq((a), (b), #a, #b, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol)                                                                   \
    do {                                                                                        \
        const double _a = (a), _b = (b);                                                        \
        if (std::fabs(_a - _b) > (tol))                                                         \
            throw t::Failure(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #a "=" + \
                             std::to_string(_a) + " not within " #tol " of " + std::to_string(_b)); \
    } while (0)
#define CHECK_THROWS(expr)                     \
    do {                                       \
        bool _threw = false;                   \
        try { expr; } catch (...) { _threw = true; } \
        CHECK(_threw);                         \
    } while (0)
