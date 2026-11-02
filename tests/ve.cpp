// ve_utils.hpp against std::, built once per ve:: target (tests/CMakeLists.txt
// passes VE_TEST_TARGET and the -m flags). Each function runs over a dense
// sweep of its range plus the edge cases, against the double precision std::
// result rounded to float; a lane fails past its ulp bound (or, where the
// result is near 0, its absolute bound). Exit 77 = this CPU can't run it.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include "ve_utils.hpp"

namespace {
    constexpr int skip = 77;
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float qnan = std::numeric_limits<float>::quiet_NaN();

    bool cpu_supports_target() {
#if defined(__AVX512BW__)
        return __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") && __builtin_cpu_supports("avx512dq") &&
            __builtin_cpu_supports("avx512vl");
#elif defined(__AVX2__)
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
#elif defined(__AVX__)
        return __builtin_cpu_supports("avx");
#elif defined(__SSE4_2__)
        return __builtin_cpu_supports("sse4.2");
#else
        return __builtin_cpu_supports("sse3");
#endif
    }

    // distance in representable floats (same sign: bit difference)
    int64_t ulps(float a, float b) {
        if (a == b)
            return 0;
        int32_t ia, ib;
        std::memcpy(&ia, &a, 4);
        std::memcpy(&ib, &b, 4);
        // map to a monotonic integer line
        int64_t const la = ia < 0 ? int64_t(int32_t(0x80000000u)) - ia : ia;
        int64_t const lb = ib < 0 ? int64_t(int32_t(0x80000000u)) - ib : ib;
        return la > lb ? la - lb : lb - la;
    }

    bool same_special(float got, float want) {
        if (std::isnan(want))
            return std::isnan(got);
        if (std::isinf(want))
            return got == want;
        return false;
    }

    struct bound {
        int64_t ulp;
        float absolute; // also fine when this close (results near 0)
    };

    int failures = 0;

    // runs f over xs (and ys, for two argument functions), lane by lane
    template<typename V, typename S>
    void check(char const* name, std::vector<float> const& xs, std::vector<float> const& ys, V&& vf, S&& sf, bound b) {
        size_t const n = xs.size();
        constexpr size_t w = size_t(ve::vector_size);
        int64_t worst = 0;
        float worst_x = 0.f, worst_y = 0.f, worst_got = 0.f, worst_want = 0.f;
        int bad = 0;
        alignas(64) float in_x[w], in_y[w], out[w];
        for (size_t i = 0; i < n; i += w) {
            for (size_t k = 0; k < w; ++k) {
                in_x[k] = xs[i + k < n ? i + k : n - 1];
                in_y[k] = ys.empty() ? 0.f : ys[i + k < n ? i + k : n - 1];
            }
            ve::fp_vector const vx = ve::load(ve::contiguous_tags<int32_t>{0}, in_x);
            ve::fp_vector const vy = ve::load(ve::contiguous_tags<int32_t>{0}, in_y);
            ve::store(ve::contiguous_tags<int32_t>{0}, out, vf(vx, vy));
            for (size_t k = 0; k < w && i + k < n; ++k) {
                float const want = float(sf(double(in_x[k]), double(in_y[k])));
                float const got = out[k];
                bool ok;
                int64_t u = 0;
                if (std::isnan(want) || std::isinf(want) || std::isnan(got) || std::isinf(got)) {
                    ok = same_special(got, want);
                    u = ok ? 0 : std::numeric_limits<int32_t>::max();
                } else {
                    u = ulps(got, want);
                    ok = u <= b.ulp || std::fabs(double(got) - double(want)) <= double(b.absolute);
                }
                if (!ok) {
                    if (bad < 5)
                        std::printf("  %s(%.9g%s%.9g): got %.9g, want %.9g (%lld ulp)\n", name, double(in_x[k]), ys.empty() ? "" : ", ",
                            ys.empty() ? 0.0 : double(in_y[k]), double(got), double(want), (long long)u);
                    ++bad;
                }
                if (ok && u > worst) {
                    worst = u;
                    worst_x = in_x[k];
                    worst_y = in_y[k];
                    worst_got = got;
                    worst_want = want;
                }
            }
        }
        std::printf("%-6s %s: %zu inputs, worst in bounds %lld ulp (at %.9g%s%.9g: %.9g vs %.9g)%s\n", bad ? "FAIL" : "ok", name, n,
            (long long)worst, double(worst_x), ys.empty() ? "" : ", ", ys.empty() ? 0.0 : double(worst_y), double(worst_got),
            double(worst_want), bad ? "" : "");
        if (bad) {
            std::printf("       %d out of bounds\n", bad);
            ++failures;
        }
    }
    template<typename V, typename S>
    void check1(char const* name, std::vector<float> const& xs, V&& vf, S&& sf, bound b) {
        check(name, xs, {}, [&](ve::fp_vector x, ve::fp_vector) { return vf(x); }, [&](double x, double) { return sf(x); }, b);
    }

    std::vector<float> linear(float lo, float hi, int n) {
        std::vector<float> v;
        v.reserve(size_t(n) + 1);
        for (int i = 0; i <= n; ++i)
            v.push_back(lo + (hi - lo) * float(i) / float(n));
        return v;
    }
    // +-[lo, hi], spaced evenly in the exponent
    std::vector<float> logarithmic(float lo, float hi, int n, bool negatives) {
        std::vector<float> v;
        double const a = std::log(double(lo)), b = std::log(double(hi));
        for (int i = 0; i <= n; ++i) {
            float const x = float(std::exp(a + (b - a) * double(i) / double(n)));
            v.push_back(x);
            if (negatives)
                v.push_back(-x);
        }
        return v;
    }
    std::vector<float> operator+(std::vector<float> a, std::vector<float> const& b) {
        a.insert(a.end(), b.begin(), b.end());
        return a;
    }
}

int main() {
    if (!cpu_supports_target()) {
        std::printf("this CPU can't run the %s build, skipped\n", VE_TEST_TARGET);
        return skip;
    }
    std::printf("ve_utils on %s, %d lanes\n", VE_TEST_TARGET, int(ve::vector_size));

    std::vector<float> const specials = {0.f, -0.f, inf, -inf, qnan};

    // trigonometry: the game's angles (a few turns), then out to 8192
    auto const angles = linear(-20.f, 20.f, 400000) + logarithmic(1e-30f, 8192.f, 200000, true) + specials;
    check1("sin", angles, [](ve::fp_vector x) { return ve::sin(x); }, [](double x) { return std::sin(x); }, {2, 1e-7f});
    check1("cos", angles, [](ve::fp_vector x) { return ve::cos(x); }, [](double x) { return std::cos(x); }, {2, 1e-7f});
    check1("sincos.sin", angles, [](ve::fp_vector x) { return ve::sincos(x).sin; }, [](double x) { return std::sin(x); }, {2, 1e-7f});
    check1("sincos.cos", angles, [](ve::fp_vector x) { return ve::sincos(x).cos; }, [](double x) { return std::cos(x); }, {2, 1e-7f});

    auto const slopes = linear(-10.f, 10.f, 200000) + logarithmic(1e-30f, 1e30f, 200000, true) + specials;
    check1("atan", slopes, [](ve::fp_vector x) { return ve::atan(x); }, [](double x) { return std::atan(x); }, {2, 0.f});

    auto const unit = linear(-1.f, 1.f, 400000) + logarithmic(1e-30f, 1.f, 100000, true) +
        std::vector<float>{1.0000001f, -1.0000001f, 2.f, -2.f} + specials;
    check1("asin", unit, [](ve::fp_vector x) { return ve::asin(x); }, [](double x) { return std::asin(x); }, {2, 0.f});
    check1("acos", unit, [](ve::fp_vector x) { return ve::acos(x); }, [](double x) { return std::acos(x); }, {2, 0.f});

    {
        // a grid of directions and lengths, the axes, and the signed zeros
        std::vector<float> ys, xs;
        auto const coords = linear(-50.f, 50.f, 700) + logarithmic(1e-20f, 1e20f, 40, true) + std::vector<float>{0.f, -0.f, inf, -inf};
        for (float y : coords)
            for (float x : coords) {
                if (std::isinf(x) && std::isinf(y))
                    continue; // documented: NaN instead of the diagonals
                ys.push_back(y);
                xs.push_back(x);
            }
        ys.push_back(qnan);
        xs.push_back(1.f);
        check("atan2", ys, xs, [](ve::fp_vector y, ve::fp_vector x) { return ve::atan2(y, x); },
            [](double y, double x) { return std::atan2(y, x); }, {2, 0.f});
    }

    // exp: denormal results flush to 0 (documented), so stop at the normals
    auto const exponents = linear(-87.3f, 88.7f, 400000) + logarithmic(1e-30f, 1.f, 100000, true) +
        std::vector<float>{-100.f, -1000.f, 89.f, 1000.f} + specials;
    check1("exp", exponents, [](ve::fp_vector x) { return ve::exp(x); },
        [](double x) {
            double const e = std::exp(x);
            return x < -87.33654475 ? 0.0 : e;
        },
        {2, 0.f});

    auto const positives = linear(0.f, 10.f, 200000) + logarithmic(1e-45f, 3e38f, 300000, false) +
        std::vector<float>{-1.f, -1e-30f, 1.f} + specials;
    check1("log", positives, [](ve::fp_vector x) { return ve::log(x); }, [](double x) { return std::log(x); }, {2, 0.f});

    {
        // the game's: bases in [0, 1] (inverse_lerp results), exponents in
        // (0, 8]; then wider bases and exponents, negative bases with whole
        // exponents, and the std:: special cases
        std::vector<float> bs, es;
        auto const bases = linear(0.f, 1.f, 1000);
        auto const powers = linear(0.05f, 8.f, 200);
        for (float b : bases)
            for (float e : powers) {
                bs.push_back(b);
                es.push_back(e);
            }
        auto const wide_bases = logarithmic(1e-3f, 1e3f, 300, true);
        auto const wide_powers = linear(-4.f, 4.f, 81);
        for (float b : wide_bases)
            for (float e : wide_powers) {
                bs.push_back(b);
                es.push_back(e);
            }
        float const special_pairs[][2] = {{0.f, 0.f}, {0.f, 2.f}, {0.f, -2.f}, {-0.f, 3.f}, {1.f, qnan}, {1.f, inf}, {qnan, 0.f},
            {-2.f, 0.5f}, {-2.f, 3.f}, {-2.f, -3.f}, {-8.f, 2.f}, {2.f, 200.f}, {2.f, -200.f}, {inf, 2.f}, {inf, -2.f}};
        for (auto const& p : special_pairs) {
            bs.push_back(p[0]);
            es.push_back(p[1]);
        }
        check("pow", bs, es, [](ve::fp_vector b, ve::fp_vector e) { return ve::pow(b, e); },
            [](double b, double e) { return std::pow(b, e); }, {2, 0.f});
    }

    std::printf(failures ? "%d function(s) failed\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
