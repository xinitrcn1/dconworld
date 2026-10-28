#pragma once

// Small vector/scalar helpers that reproduce Unity's (Mathf/Vector2) and
// RWCustom.Custom semantics exactly. Movement code is very sensitive to these
// edge cases (Mathf.Sign(0) == 1, Lerp clamps t, InverseLerp(a, a) == 0, ...)
// so do NOT replace them with "nicer" std equivalents.

#include <cmath>
#include <cstdint>
#include <limits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace rw {
    using vec2 = glm::vec2;
    using vec3 = glm::vec3;
    using ivec2 = glm::ivec2;

    // UnityEngine.Vector2 instance members, as free functions over glm types
    inline float magnitude(vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }
    constexpr float sqr_magnitude(vec2 v) noexcept { return v.x * v.x + v.y * v.y; }
    // Vector2.normalized: zero below 1e-5 (glm::normalize would return NaN)
    inline vec2 normalized(vec2 v) noexcept {
        float const m = magnitude(v);
        return m > 1e-5f ? vec2{v.x / m, v.y / m} : vec2{0.f, 0.f};
    }
    constexpr vec2 to_vec2(ivec2 v) noexcept { return vec2{float(v.x), float(v.y)}; }

    // UnityEngine.Mathf
    namespace mathf {
        inline constexpr float pi = 3.1415927f;
        inline constexpr float deg2rad = 0.017453292f;
        constexpr float clamp01(float v) noexcept { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
        constexpr float clamp(float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
        constexpr float lerp(float a, float b, float t) noexcept { return a + (b - a) * clamp01(t); }
        constexpr float inverse_lerp(float a, float b, float v) noexcept {
            return a != b ? clamp01((v - a) / (b - a)) : 0.f;
        }
        // Mathf.Sign returns 1 for 0 (unlike System.Math.Sign)
        constexpr float sign(float v) noexcept { return v >= 0.f ? 1.f : -1.f; }
        inline float pow(float b, float e) noexcept { return std::pow(b, e); }
        constexpr float max(float a, float b) noexcept { return a > b ? a : b; }
        constexpr float min(float a, float b) noexcept { return a < b ? a : b; }
        constexpr int32_t max(int32_t a, int32_t b) noexcept { return a > b ? a : b; }
        constexpr int32_t min(int32_t a, int32_t b) noexcept { return a < b ? a : b; }
    }
    // System.Math.Sign (returns 0 for 0)
    constexpr int32_t math_sign(int32_t v) noexcept { return (v > 0) - (v < 0); }

    // UnityEngine.Vector2 statics
    inline float distance(vec2 a, vec2 b) noexcept { return magnitude(a - b); }
    constexpr float dot(vec2 a, vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
    inline vec2 clamp_magnitude(vec2 v, float max_len) noexcept {
        float const sq = sqr_magnitude(v);
        if (sq > max_len * max_len) {
            float const m = std::sqrt(sq);
            return vec2{v.x / m * max_len, v.y / m * max_len};
        }
        return v;
    }
    inline vec2 lerp(vec2 a, vec2 b, float t) noexcept {
        t = mathf::clamp01(t);
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
    }
    // Vector2.Angle, in degrees
    inline float angle(vec2 from, vec2 to) noexcept {
        float const d = std::sqrt(sqr_magnitude(from) * sqr_magnitude(to));
        if (d < 1e-15f)
            return 0.f;
        float const c = mathf::clamp(dot(from, to) / d, -1.f, 1.f);
        return std::acos(c) * 57.29578f;
    }

    // RWCustom.Custom
    namespace custom {
        inline vec2 dir_vec(vec2 p1, vec2 p2) noexcept {
            if (p1 == p2)
                return {0.f, 1.f};
            return normalized(p2 - p1);
        }
        inline vec2 perpendicular_vector(vec2 v) noexcept {
            v = normalized(v);
            return {-v.y, v.x};
        }
        // Note: Custom.PerpendicularVector(a, b) uses (a - b), not DirVec(a, b)
        inline vec2 perpendicular_vector(vec2 a, vec2 b) noexcept { return perpendicular_vector(a - b); }
        inline vec2 deg_to_vec(float ang) noexcept {
            return {std::sin(ang * mathf::deg2rad), std::cos(ang * mathf::deg2rad)};
        }
        inline bool dist_less(vec2 a, vec2 b, float dst) noexcept { return magnitude(a - b) < dst; }
        constexpr int32_t int_clamp(int32_t v, int32_t lo, int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
        constexpr float lerp_map(float v, float from_a, float to_a, float from_b, float to_b) noexcept {
            float const t = mathf::inverse_lerp(from_a, to_a, v);
            return from_b + (to_b - from_b) * t;
        }
        inline float lerp_map(float v, float from_a, float to_a, float from_b, float to_b, float exponent) noexcept {
            float const t = std::pow(mathf::inverse_lerp(from_a, to_a, v), exponent);
            return from_b + (to_b - from_b) * t;
        }
        inline constexpr ivec2 eight_directions[8] = {
            {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}
        };
        inline constexpr ivec2 four_directions[4] = {
            {-1, 0}, {0, -1}, {1, 0}, {0, 1}
        };
    }

    // UnityEngine.Random replacement (deterministic xorshift, seedable)
    struct rng {
        uint32_t state = 0x9E3779B9u;
        uint32_t next() noexcept {
            uint32_t x = state;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            return state = x;
        }
        // Random.value: [0, 1]
        float value() noexcept { return float(next() >> 8) * (1.f / 16777215.f); }
        // Random.Range(int, int): [lo, hi)
        int32_t range(int32_t lo, int32_t hi) noexcept {
            return hi <= lo ? lo : lo + int32_t(next() % uint32_t(hi - lo));
        }
    };
}
