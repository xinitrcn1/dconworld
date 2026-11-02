#pragma once

// transcendental functions over dcon's ve::fp_vector, for whole-world vector
// passes: what the scalar logic calls through std:: (sin, cos, atan2, acos,
// exp, pow), lane for lane. The method is SVML's / Cephes': reduce the
// argument to a small range exactly (Cody-Waite), evaluate a minimax
// polynomial there, then undo the reduction. No SVML (_mm_cos_ps & co. are
// Intel/MSVC only); everything is written in ve:: operations, except the
// float <-> int bit casts and the int shifts, which ve:: lacks and are
// spelled out per target below.
//
// Accuracy against std:: (tests/ve.cpp checks it on every ve:: target):
// - sin, cos: <= 2 ulp, or 1e-7 absolute near the zeros, for |x| < 8192;
//   the reduction loses bits past that (still finite, error grows with |x|)
// - atan, atan2, asin, acos, exp, log, pow: <= 2 ulp
// - exp (and pow) flush to 0 under e^-87.3 (no denormals)
// Edge cases follow std:: where noted; NaN in gives NaN out.
#include <cstdint>

#include "ve.hpp"

// (ve.hpp #undefs its RELEASE_INLINE at the end)
#if defined(NDEBUG)
#define VE_UTILS_INLINE inline __attribute__((always_inline))
#else
#define VE_UTILS_INLINE inline
#endif

namespace ve {
    // --- the per-target bits: float <-> int reinterpretation, shifts -----

#if defined(__AVX512BW__)
    VE_UTILS_INLINE int_vector bits_of(fp_vector v) noexcept { return int_vector{_mm512_castps_si512(v.value)}; }
    VE_UTILS_INLINE fp_vector float_of(int_vector v) noexcept { return fp_vector{_mm512_castsi512_ps(v.value)}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_left(int_vector v) noexcept { return int_vector{_mm512_slli_epi32(v.value, N)}; }
    // logical (zeros in)
    template<int N>
    VE_UTILS_INLINE int_vector shift_right(int_vector v) noexcept { return int_vector{_mm512_srli_epi32(v.value, N)}; }
#elif defined(__AVX2__)
    VE_UTILS_INLINE int_vector bits_of(fp_vector v) noexcept { return int_vector{_mm256_castps_si256(v.value)}; }
    VE_UTILS_INLINE fp_vector float_of(int_vector v) noexcept { return fp_vector{_mm256_castsi256_ps(v.value)}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_left(int_vector v) noexcept { return int_vector{_mm256_slli_epi32(v.value, N)}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_right(int_vector v) noexcept { return int_vector{_mm256_srli_epi32(v.value, N)}; }
#elif defined(__AVX__)
    // ve_avx's int_vector is two 128 bit halves (AVX has no 256 bit int ops)
    VE_UTILS_INLINE int_vector bits_of(fp_vector v) noexcept { return int_vector{_mm256_castps_si256(v.value)}; }
    VE_UTILS_INLINE fp_vector float_of(int_vector v) noexcept { return fp_vector{_mm256_castsi256_ps(v.to_256_packed_int())}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_left(int_vector v) noexcept {
        return int_vector{_mm_slli_epi32(v.value_low, N), _mm_slli_epi32(v.value_high, N)};
    }
    template<int N>
    VE_UTILS_INLINE int_vector shift_right(int_vector v) noexcept {
        return int_vector{_mm_srli_epi32(v.value_low, N), _mm_srli_epi32(v.value_high, N)};
    }
#else
    // ve_sse and ve_sse3
    VE_UTILS_INLINE int_vector bits_of(fp_vector v) noexcept { return int_vector{_mm_castps_si128(v.value)}; }
    VE_UTILS_INLINE fp_vector float_of(int_vector v) noexcept { return fp_vector{_mm_castsi128_ps(v.value)}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_left(int_vector v) noexcept { return int_vector{_mm_slli_epi32(v.value, N)}; }
    template<int N>
    VE_UTILS_INLINE int_vector shift_right(int_vector v) noexcept { return int_vector{_mm_srli_epi32(v.value, N)}; }
#endif

    // a * b - p exactly (p = a * b rounded): one FMA where there is one; else
    // Dekker's split, which is exact only because nothing can fuse it
#if defined(__AVX512F__)
    VE_UTILS_INLINE fp_vector product_error(fp_vector a, fp_vector b, fp_vector p) noexcept { return fp_vector{_mm512_fmsub_ps(a.value, b.value, p.value)}; }
#elif defined(__FMA__) && defined(__AVX__)
    VE_UTILS_INLINE fp_vector product_error(fp_vector a, fp_vector b, fp_vector p) noexcept { return fp_vector{_mm256_fmsub_ps(a.value, b.value, p.value)}; }
#elif defined(__FMA__)
    VE_UTILS_INLINE fp_vector product_error(fp_vector a, fp_vector b, fp_vector p) noexcept { return fp_vector{_mm_fmsub_ps(a.value, b.value, p.value)}; }
#else
    VE_UTILS_INLINE fp_vector product_error(fp_vector a, fp_vector b, fp_vector p) noexcept {
        auto const split = [](fp_vector v, fp_vector& hi, fp_vector& lo) {
            fp_vector const c = fp_vector{4097.f} * v;
            hi = c - (c - v);
            lo = v - hi;
        };
        fp_vector ah, al, bh, bl;
        split(a, ah, al);
        split(b, bh, bl);
        return (((ah * bh - p) + ah * bl) + al * bh) + al * bl;
    }
#endif

    namespace detail {
        inline constexpr int32_t sign_bit = int32_t(0x80000000u);
        inline constexpr float pi = 3.14159265358979f, half_pi = 1.5707963267948966f, quarter_pi = 0.7853981633974483f;
        // ln 2 in two parts: n * ln2_hi is exact for |n| < 2^15
        inline constexpr float ln2_hi = 0.693359375f, ln2_lo = -2.12194440e-4f;

        VE_UTILS_INLINE fp_vector infinity() noexcept { return float_of(int_vector{0x7F800000}); }
        VE_UTILS_INLINE fp_vector quiet_nan() noexcept { return float_of(int_vector{0x7FC00000}); }
        VE_UTILS_INLINE mask_vector is_nan(fp_vector v) noexcept { return !(v == v); } // (ve's != is ordered)

        // a's magnitude with b's sign
        VE_UTILS_INLINE fp_vector copy_sign(fp_vector a, fp_vector b) noexcept {
            return float_of((bits_of(a) & int_vector{~sign_bit}) | (bits_of(b) & int_vector{sign_bit}));
        }
        // the sign bit set (so -0.f too)
        VE_UTILS_INLINE mask_vector sign_set(fp_vector v) noexcept {
            return bits_of(v) < int_vector{0};
        }
        // the polynomial c[0] + c[1] x + ... (Horner, highest first)
        template<size_t N>
        VE_UTILS_INLINE fp_vector poly(fp_vector x, float const (&c)[N]) noexcept {
            fp_vector r{c[N - 1]};
            for (size_t i = N - 1; i-- > 0;)
                r = multiply_and_add(r, x, fp_vector{c[i]});
            return r;
        }
        // s = a + b rounded, err what the rounding lost (Knuth's two-sum)
        VE_UTILS_INLINE fp_vector sum_error(fp_vector a, fp_vector b, fp_vector s) noexcept {
            fp_vector const bb = s - a;
            return (a - (s - bb)) + (b - bb);
        }

        // sin and cos of r in [-pi/4, pi/4] (Cephes sinf / cosf)
        VE_UTILS_INLINE fp_vector sin_kernel(fp_vector r, fp_vector r2) noexcept {
            static constexpr float s[] = {-1.6666654611e-1f, 8.3321608736e-3f, -1.9515295891e-4f};
            return multiply_and_add(poly(r2, s) * r2, r, r);
        }
        VE_UTILS_INLINE fp_vector cos_kernel(fp_vector r2) noexcept {
            static constexpr float c[] = {4.166664568298827e-2f, -1.388731625493765e-3f, 2.443315711809948e-5f};
            fp_vector const r4 = r2 * r2;
            return multiply_and_add(poly(r2, c), r4, negate_multiply_and_add(fp_vector{0.5f}, r2, fp_vector{1.f}));
        }
        // x - q pi/2, q the nearest quadrant, pi/2 in three parts so the
        // first two products are exact (Cody-Waite)
        struct reduced {
            fp_vector r;
            int_vector q;
        };
        VE_UTILS_INLINE reduced reduce_half_pi(fp_vector x) noexcept {
            int_vector const q = to_int(x * fp_vector{0.63661977236758134f}); // round to nearest
            fp_vector const qf = to_float(q);
            fp_vector r = negate_multiply_and_add(qf, fp_vector{1.5703125f}, x);
            r = negate_multiply_and_add(qf, fp_vector{4.837512969970703125e-4f}, r);
            r = negate_multiply_and_add(qf, fp_vector{7.54978995489188216e-8f}, r);
            return {r, q};
        }
        // sin of x where x = r + q pi/2
        VE_UTILS_INLINE fp_vector sin_quadrant(fp_vector r, int_vector q) noexcept {
            fp_vector const r2 = r * r;
            fp_vector const v = select((q & int_vector{1}) != int_vector{0}, cos_kernel(r2), sin_kernel(r, r2));
            return select((q & int_vector{2}) != int_vector{0}, -v, v);
        }

        // atan(n / d) for n, d >= 0 (Cephes atanf), reduced to |x| <=
        // tan(pi/8) on n and d themselves, so one rounded division:
        // over tan(3pi/8) pi/2 + atan(-d / n), over tan(pi/8)
        // pi/4 + atan((n - d) / (n + d))
        VE_UTILS_INLINE fp_vector atan_ratio(fp_vector n, fp_vector d) noexcept {
            mask_vector const big = n > fp_vector{2.414213562373095f} * d;
            mask_vector const mid = n > fp_vector{0.4142135623730950f} * d;
            fp_vector const num = select(big, -d, select(mid, n - d, n));
            fp_vector const den = select(big, n, select(mid, n + d, d));
            fp_vector const x = num / den;
            fp_vector const y0 = select(big, fp_vector{half_pi}, select(mid, fp_vector{quarter_pi}, fp_vector{0.f}));
            static constexpr float c[] = {-3.33329491539e-1f, 1.99777106478e-1f, -1.38776856032e-1f, 8.05374449538e-2f};
            fp_vector const z = x * x;
            return y0 + multiply_and_add(poly(z, c) * z, x, x);
        }

        // asin of s in [0, 0.5], from z = s * s (Cephes asinf)
        VE_UTILS_INLINE fp_vector asin_kernel(fp_vector s, fp_vector z) noexcept {
            static constexpr float c[] = {1.6666752422e-1f, 7.4953002686e-2f, 4.5470025998e-2f, 2.4181311049e-2f, 4.2163199048e-2f};
            return multiply_and_add(poly(z, c) * z, s, s);
        }
        // for |x| over 0.5, asin(|x|) = pi/2 - 2 asin(sqrt((1 - |x|) / 2));
        // `big` says which, p the kernel's result
        struct asin_parts {
            mask_vector big;
            fp_vector p;
        };
        VE_UTILS_INLINE asin_parts asin_split(fp_vector x) noexcept {
            fp_vector const a = abs(x);
            mask_vector const big = a > fp_vector{0.5f};
            fp_vector const half_rest = fp_vector{0.5f} * (fp_vector{1.f} - a);
            fp_vector const z = select(big, half_rest, a * a);
            fp_vector const s = select(big, sqrt(half_rest), a); // NaN past |x| > 1, like std::
            return {big, asin_kernel(s, z)};
        }

        // e^(x + x_lo), x_lo a correction under x's last bit (Cephes expf);
        // 0 under -87.3 (no denormals), inf over 88.7
        VE_UTILS_INLINE fp_vector exp_with(fp_vector x, fp_vector x_lo) noexcept {
            fp_vector const lo{-87.33654475f}, hi{88.72283905f};
            fp_vector const c = min(max(x, lo), hi);
            // x = n ln2 + r
            int_vector const n = to_int(c * fp_vector{1.44269504088896341f});
            fp_vector const nf = to_float(n);
            fp_vector r = negate_multiply_and_add(nf, fp_vector{ln2_hi}, c);
            r = negate_multiply_and_add(nf, fp_vector{ln2_lo}, r) + x_lo;
            static constexpr float p[] = {5.0000001201e-1f, 1.6666665459e-1f, 4.1665795894e-2f, 8.3334519073e-3f, 1.3981999507e-3f, 1.9875691500e-4f};
            fp_vector const e = multiply_and_add(poly(r, p), r * r, r + fp_vector{1.f});
            // * 2^n in two halves: n reaches 128 at the top, past the exponent
            int_vector const h = to_int(floor(nf * fp_vector{0.5f}));
            fp_vector const scaled = e * float_of(shift_left<23>(h + int_vector{127})) * float_of(shift_left<23>(n - h + int_vector{127}));
            fp_vector const out = select(x < lo, fp_vector{0.f}, select(x > hi, infinity(), scaled));
            return select(is_nan(x), x, out);
        }

        // x = 2^e (1 + f), f in [sqrt(.5) - 1, sqrt(2) - 1) exactly, for
        // finite x > 0 (denormals too)
        struct log_parts {
            fp_vector e;
            fp_vector f;
        };
        VE_UTILS_INLINE log_parts log_split(fp_vector x) noexcept {
            mask_vector const tiny = x < fp_vector{1.17549435e-38f};
            fp_vector const xs = select(tiny, x * fp_vector{8388608.f}, x); // 2^23
            int_vector const bits = bits_of(xs);
            // m in [0.5, 1)
            int_vector e = shift_right<23>(bits) - int_vector{126};
            e = select(tiny, e - int_vector{23}, e);
            fp_vector const m = float_of((bits & int_vector{0x007FFFFF}) | int_vector{0x3F000000});
            mask_vector const low = m < fp_vector{0.707106781186547524f};
            e = select(low, e - int_vector{1}, e);
            return {to_float(e), select(low, m + m, m) - fp_vector{1.f}}; // (exact: Sterbenz)
        }
        // log(1 + f) - f + f^2 / 2 (Cephes logf's polynomial)
        VE_UTILS_INLINE fp_vector log_tail(fp_vector f, fp_vector z) noexcept {
            static constexpr float p[] = {3.3333331174e-1f, -2.4999993993e-1f, 2.0000714765e-1f, -1.6668057665e-1f, 1.4249322787e-1f,
                -1.2420140846e-1f, 1.1676998740e-1f, -1.1514610310e-1f, 7.0376836292e-2f};
            return poly(f, p) * f * z;
        }
        // ln x as hi + lo, ~2^-29 relative: what pow needs so e * ln b keeps
        // its bits (SVML's powf does the same in double-float)
        struct twofold {
            fp_vector hi;
            fp_vector lo;
        };
        VE_UTILS_INLINE twofold log_twofold(fp_vector x) noexcept {
            auto const [e, f] = log_split(x);
            // e ln2_hi + f - f^2 / 2 summed with the rounding errors kept,
            // then e ln2_lo + the tail (small: their roundings don't matter)
            fp_vector const q = f * f;
            fp_vector const q_err = product_error(f, f, q);
            fp_vector const a = e * fp_vector{ln2_hi}; // exact
            fp_vector const s1 = a + f;
            fp_vector const half_q = fp_vector{-0.5f} * q;  // exact
            fp_vector const s2 = s1 + half_q;
            fp_vector const lo = sum_error(a, f, s1) + sum_error(s1, half_q, s2) - fp_vector{0.5f} * q_err +
                multiply_and_add(e, fp_vector{ln2_lo}, log_tail(f, q));
            fp_vector const hi = s2 + lo;
            return {hi, lo - (hi - s2)};
        }
    }

    // --- trigonometry, radians ------------------------------------------

    VE_UTILS_INLINE fp_vector sin(fp_vector x) noexcept {
        auto const [r, q] = detail::reduce_half_pi(x);
        return detail::sin_quadrant(r, q);
    }
    VE_UTILS_INLINE fp_vector cos(fp_vector x) noexcept {
        // cos(x) = sin(x + pi/2): one quadrant on
        auto const [r, q] = detail::reduce_half_pi(x);
        return detail::sin_quadrant(r, q + int_vector{1});
    }
    // both from one reduction
    struct sin_cos_result {
        fp_vector sin;
        fp_vector cos;
    };
    VE_UTILS_INLINE sin_cos_result sincos(fp_vector x) noexcept {
        auto const [r, q] = detail::reduce_half_pi(x);
        fp_vector const r2 = r * r;
        fp_vector const s = detail::sin_kernel(r, r2), c = detail::cos_kernel(r2);
        mask_vector const odd = (q & int_vector{1}) != int_vector{0};
        fp_vector const sv = select(odd, c, s), cv = select(odd, s, c);
        return {
            select((q & int_vector{2}) != int_vector{0}, -sv, sv),
            select(((q + int_vector{1}) & int_vector{2}) != int_vector{0}, -cv, cv)};
    }

    VE_UTILS_INLINE fp_vector atan(fp_vector x) noexcept {
        return detail::copy_sign(detail::atan_ratio(abs(x), fp_vector{1.f}), x);
    }
    // std::atan2's quadrants and signed zeros; (+-inf, +-inf) gives NaN
    // instead of +-pi/4 and +-3pi/4
    VE_UTILS_INLINE fp_vector atan2(fp_vector y, fp_vector x) noexcept {
        fp_vector const ay = abs(y);
        fp_vector a = detail::atan_ratio(ay, abs(x));            // x 0: pi/2
        a = select(ay == fp_vector{0.f}, fp_vector{0.f}, a);     // 0/0
        a = select(detail::sign_set(x), fp_vector{detail::pi} - a, a);
        return detail::copy_sign(a, y);
    }

    VE_UTILS_INLINE fp_vector asin(fp_vector x) noexcept {
        auto const [big, p] = detail::asin_split(x);
        fp_vector const a = select(big, fp_vector{detail::half_pi} - (p + p), p);
        return detail::copy_sign(a, x);
    }
    VE_UTILS_INLINE fp_vector acos(fp_vector x) noexcept {
        // |x| > 0.5: acos(|x|) = 2 asin(sqrt((1 - |x|) / 2)), acos(-|x|) = pi - that;
        // else pi/2 - asin(x). Never pi/2 - asin(x) near 1, it cancels
        auto const [big, p] = detail::asin_split(x);
        fp_vector const twice = p + p;
        fp_vector const far = select(detail::sign_set(x), fp_vector{detail::pi} - twice, twice);
        fp_vector const near = fp_vector{detail::half_pi} - detail::copy_sign(p, x);
        return select(big, far, near);
    }

    // --- exponentials ---------------------------------------------------

    // e^x; 0 under -87.3 (no denormals), inf over 88.7
    VE_UTILS_INLINE fp_vector exp(fp_vector x) noexcept {
        return detail::exp_with(x, fp_vector{0.f});
    }

    // ln x (Cephes logf); 0 gives -inf, negatives NaN, inf inf
    VE_UTILS_INLINE fp_vector log(fp_vector x) noexcept {
        auto const [e, f] = detail::log_split(x);
        fp_vector const z = f * f;
        fp_vector y = multiply_and_add(e, fp_vector{detail::ln2_lo}, detail::log_tail(f, z));
        y = negate_multiply_and_add(fp_vector{0.5f}, z, y);
        fp_vector out = multiply_and_add(e, fp_vector{detail::ln2_hi}, f + y);
        out = select(x == detail::infinity(), x, out);
        out = select(x == fp_vector{0.f}, -detail::infinity(), out);
        return select(x < fp_vector{0.f} || detail::is_nan(x), detail::quiet_nan(), out);
    }

    // b^e as std::pow: e 0 or b 1 gives 1; b +-0 gives 0 (e > 0) or inf
    // (e < 0), -0 keeping an odd e's sign; a negative b takes e's parity if
    // e is whole, else NaN. e ln|b| is carried in double-float, so the
    // result stays within a few ulp however large it is
    VE_UTILS_INLINE fp_vector pow(fp_vector b, fp_vector e) noexcept {
        fp_vector const a = abs(b);
        auto const [hi, lo] = detail::log_twofold(a);
        fp_vector const t = e * hi;
        fp_vector t_lo = multiply_and_add(e, lo, product_error(e, hi, t));
        t_lo = select(detail::is_nan(t_lo), fp_vector{0.f}, t_lo); // (t is inf or NaN then)
        fp_vector m = detail::exp_with(t, t_lo);
        // |b| 0 or inf: log_split's not for them
        mask_vector const grows = e > fp_vector{0.f};
        m = select(a == fp_vector{0.f}, select(grows, fp_vector{0.f}, detail::infinity()), m);
        m = select(a == detail::infinity(), select(grows, detail::infinity(), fp_vector{0.f}), m);
        m = select(detail::is_nan(b), b, m);

        mask_vector const whole = floor(e) == e;
        // whole |e| past 2^24 are even; to_int's overflow value is too
        mask_vector const odd = whole && (to_int(e) & int_vector{1}) != int_vector{0};
        mask_vector const negative = detail::sign_set(b);
        fp_vector out = select(negative && odd, -m, m);
        out = select(b < fp_vector{0.f} && !whole, detail::quiet_nan(), out);
        return select(e == fp_vector{0.f} || b == fp_vector{1.f}, fp_vector{1.f}, out);
    }

    // --- arrays ---------------------------------------------------------

    // out[i] = f(in[i]) for i in [0, n), ve::vector_size at a time (the last
    // lanes repeat in[n - 1]: every element goes through the same vector
    // code, wherever the lanes' boundaries fall). `in` and `out` may alias
    template<typename F>
    inline void transform(float const* in, float* out, size_t n, F&& f) {
        constexpr size_t lanes = size_t(vector_size);
        alignas(64) float buf[lanes];
        for (size_t base = 0; base < n; base += lanes) {
            for (size_t k = 0; k < lanes; ++k)
                buf[k] = in[base + k < n ? base + k : n - 1];
            store(contiguous_tags<int32_t>{0}, buf, fp_vector{f(load(contiguous_tags<int32_t>{0}, buf))});
            for (size_t k = 0; k < lanes && base + k < n; ++k)
                out[base + k] = buf[k];
        }
    }
    // the same, in place
    template<typename F>
    inline void transform(float* values, size_t n, F&& f) {
        transform(values, values, n, f);
    }
}

#undef VE_UTILS_INLINE
