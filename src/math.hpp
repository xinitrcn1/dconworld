#pragma once

// Small vector/scalar helpers that reproduce Unity's (Mathf/Vector2) and
// RWCustom.Custom semantics exactly. Movement code is very sensitive to these
// edge cases (Mathf.Sign(0) == 1, Lerp clamps t, InverseLerp(a, a) == 0, ...)
// so do NOT replace them with "nicer" std equivalents.

#include <cmath>
#include <cstdint>
#include <source_location>
#include <limits>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

namespace rw {
    using vec2 = glm::vec2;
    using vec3 = glm::vec3;
    using vec4 = glm::vec4;
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
    // Vector3.Slerp on 2D vectors: the angle and the length interpolated
    inline vec2 slerp(vec2 a, vec2 b, float t) noexcept {
        t = mathf::clamp01(t);
        float const la = magnitude(a), lb = magnitude(b);
        if (la < 1e-5f || lb < 1e-5f)
            return lerp(a, b, t);
        float const aa = std::atan2(a.y, a.x);
        float da = std::atan2(b.y, b.x) - aa;
        while (da > mathf::pi)
            da -= 2.f * mathf::pi;
        while (da < -mathf::pi)
            da += 2.f * mathf::pi;
        float const ang = aa + da * t;
        return vec2{std::cos(ang), std::sin(ang)} * mathf::lerp(la, lb, t);
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
        // Custom.HSL2RGB
        inline vec3 hsl2rgb(float h, float s, float l) noexcept {
            h = h - std::floor(h);
            auto const f = [&](float n) {
                float const k = std::fmod(n + h * 12.f, 12.f);
                float const a = s * mathf::min(l, 1.f - l);
                return l - a * mathf::max(-1.f, mathf::min(mathf::min(k - 3.f, 9.f - k), 1.f));
            };
            return vec3{f(0.f), f(8.f), f(4.f)};
        }
        inline float vec_to_deg(vec2 v) noexcept { return std::atan2(v.x, v.y) / 6.2831855f * 360.f; }
        // AimFromOneVectorToAnother: degrees, clockwise from up
        inline float aim(vec2 p1, vec2 p2) noexcept { return vec_to_deg(p2 - p1); }
        inline bool dist_less(vec2 a, vec2 b, float dst) noexcept { return magnitude(a - b) < dst; }
        inline vec2 bezier(vec2 a, vec2 ca, vec2 b, vec2 cb, float f) noexcept {
            vec2 const m = lerp(ca, cb, f);
            ca = lerp(a, ca, f);
            cb = lerp(cb, b, f);
            ca = lerp(ca, m, f);
            cb = lerp(m, cb, f);
            return lerp(ca, cb, f);
        }
        // the joint of a two piece limb from `va` reaching for `vc`
        inline vec2 inverse_kinematic(vec2 va, vec2 vc, float a, float b, float flip) noexcept {
            float const d = distance(va, vc);
            float const ang = std::acos(mathf::clamp((d * d + a * a - b * b) / (2.f * d * a), 0.2f, 0.98f)) * (flip * 180.f / 3.1415927f);
            return va + deg_to_vec(aim(va, vc) + ang) * a;
        }
        constexpr int32_t int_clamp(int32_t v, int32_t lo, int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
        constexpr float lerp_map(float v, float from_a, float to_a, float from_b, float to_b) noexcept {
            float const t = mathf::inverse_lerp(from_a, to_a, v);
            return from_b + (to_b - from_b) * t;
        }
        inline float lerp_map(float v, float from_a, float to_a, float from_b, float to_b, float exponent) noexcept {
            float const t = std::pow(mathf::inverse_lerp(from_a, to_a, v), exponent);
            return from_b + (to_b - from_b) * t;
        }
        // Custom.LerpAndTick: a lerp, then at least `tick` closer
        constexpr float lerp_and_tick(float from, float to, float l, float tick) noexcept {
            float const v = mathf::lerp(from, to, l);
            return v < to ? mathf::min(v + tick, to) : mathf::max(v - tick, to);
        }
        // Custom.SCurve
        constexpr float s_curve(float x, float k) noexcept {
            x = x * 2.f - 1.f;
            if (x < 0.f) {
                x = 1.f + x < 0.f ? -(1.f + x) : 1.f + x;
                return k * x / (k - x + 1.f) * 0.5f;
            }
            k = -1.f - k;
            return 0.5f + k * x / (k - x + 1.f) * 0.5f;
        }
        // Custom.DistanceToLine: signed distance of `v` to the line through l2, l1
        inline float distance_to_line(vec2 v, vec2 l2, vec2 l1) noexcept {
            return ((l2.y - l1.y) * v.x - (l2.x - l1.x) * v.y + l2.x * l1.y - l2.y * l1.x) / std::sqrt((l2.y - l1.y) * (l2.y - l1.y) + (l2.x - l1.x) * (l2.x - l1.x));
        }
        // Custom.RNV: a random unit vector from `r` (rw::rng or rw::unity_random)
        template<typename R>
        vec2 rnv(R& r) noexcept { return deg_to_vec(r.value() * 360.f); }
        // Custom.ClampedRandomVariation (with Custom.RandomDeviation) from `r`
        template<typename R>
        float clamped_random_variation(float base, float max_deviation, float k, R& r) noexcept {
            float const dev = s_curve(r.value() * 0.5f, k) * 2.f * (r.value() < 0.5f ? 1.f : -1.f);
            return mathf::clamp(base + dev * max_deviation, 0.f, 1.f);
        }
        // Custom.WrappedRandomVariation (with Custom.RandomDeviation) from `r`
        template<typename R>
        float wrapped_random_variation(float base, float max_deviation, float k, R& r) noexcept {
            float const dev = s_curve(r.value() * 0.5f, k) * 2.f * (r.value() < 0.5f ? 1.f : -1.f);
            float const num = base + dev * max_deviation + 1.f;
            return num - std::floor(num);
        }
        // Custom.RGB2HSL: (hue, saturation, lightness)
        inline vec3 rgb2hsl(vec3 c) noexcept {
            float const mx = mathf::max(mathf::max(c.x, c.y), c.z);
            float const mn = mathf::min(mathf::min(c.x, c.y), c.z);
            float const l = (mx + mn) / 2.f;
            if (mx == mn)
                return vec3{0.f, 0.f, l};
            float const d = mx - mn;
            float const s = l > 0.5f ? d / (2.f - mx - mn) : d / (mx + mn);
            float h;
            if (mx == c.x)
                h = (c.y - c.z) / d + (c.y < c.z ? 6.f : 0.f);
            else if (mx == c.y)
                h = (c.z - c.x) / d + 2.f;
            else
                h = (c.x - c.y) / d + 4.f;
            return vec3{h / 6.f, s, l};
        }
        // Custom.ClosestPointOnLine (the infinite line through a, b)
        inline vec2 closest_point_on_line(vec2 a, vec2 b, vec2 p) noexcept {
            vec2 const ap = p - a, ab = b - a;
            float const t = (ap.x * ab.x + ap.y * ab.y) / (ab.x * ab.x + ab.y * ab.y);
            return a + ab * t;
        }
        // Custom.MoveTowards
        inline vec2 move_towards(vec2 a, vec2 b, float speed) noexcept {
            if (dist_less(a, b, speed))
                return b;
            return a + dir_vec(a, b) * speed;
        }
        // IntVector2.ClampAtOne
        constexpr ivec2 clamp_at_one(ivec2 v) noexcept {
            return ivec2{v.x < -1 ? -1 : (v.x > 1 ? 1 : v.x), v.y < -1 ? -1 : (v.y > 1 ? 1 : v.y)};
        }
        // Custom.Decimal: the fractional part
        inline float decimal(float f) noexcept { return f - std::floor(f); }
        // Custom.ManhattanDistance
        constexpr int32_t manhattan_distance(ivec2 a, ivec2 b) noexcept {
            return (a.x > b.x ? a.x - b.x : b.x - a.x) + (a.y > b.y ? a.y - b.y : b.y - a.y);
        }
        inline constexpr ivec2 eight_directions[8] = {
            {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}
        };
        inline constexpr ivec2 eight_directions_diagonals_last[8] = {
            {-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {1, 1}, {-1, 1}
        };
        inline constexpr ivec2 eight_directions_and_zero[9] = {
            {0, 0}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}, {1, 0}, {1, 1}, {0, 1}, {-1, 1}
        };
        inline constexpr ivec2 four_directions[4] = {
            {-1, 0}, {0, -1}, {1, 0}, {0, 1}
        };
    }

    // The stateless generator (docs/PIPELINE.md). A roll is a pure function
    // of the world's seed (immutable, set when the world is made), the tick
    // (passed explicitly), the ids or properties that tell the roller apart
    // (dcon ids' .value, a creature's seed), the call site, and which draw of
    // the call it is. No generator state is stored anywhere, so no roll
    // depends on another's and any object, lane or thread can roll in any
    // order. Each input is multiplied by its own odd constant before they're
    // XORed (a bare tick ^ id gives tick 1 / id 2 and tick 2 / id 1 the same
    // key), then mixed by lowbias32's finalizer. keyed_random.hpp has the ve::
    // versions, lane for lane the same bits
    namespace keyed {
        inline constexpr uint32_t k_tick = 0x9E3779B1u;
        inline constexpr uint32_t k_id = 0x85EBCA77u;
        inline constexpr uint32_t k_id2 = 0x27D4EB2Fu;
        inline constexpr uint32_t k_salt = 0x165667B1u;
        inline constexpr uint32_t k_draw = 0xC2B2AE3Du;
        constexpr uint32_t mix(uint32_t x) noexcept {
            x ^= x >> 16;
            x *= 0x7FEB352Du;
            x ^= x >> 15;
            x *= 0x846CA68Bu;
            x ^= x >> 16;
            return x;
        }
        constexpr uint32_t key(uint32_t seed, uint32_t tick, uint32_t id, uint32_t salt) noexcept {
            return seed ^ tick * k_tick ^ id * k_id ^ salt * k_salt;
        }
        // the n-th draw from a key
        constexpr uint32_t draw(uint32_t key, uint32_t n) noexcept { return mix(key ^ n * k_draw); }
        // [0, 1], UnityEngine.Random.value's scale
        constexpr float to_value(uint32_t bits) noexcept { return float(bits >> 8) * (1.f / 16777215.f); }
    }

    // two ids (or properties) as one key input: an object and the one it rolls about
    constexpr uint32_t ids(uint32_t a, uint32_t b) noexcept { return a ^ b * keyed::k_id2; }

    // the call site's salt: its file and line, so two rolls of a tick at two
    // places are unrelated (what rw::random keys by)
    constexpr uint32_t site(std::source_location const l = std::source_location::current()) noexcept {
        uint32_t h = 2166136261u;
        for (char const* c = l.file_name(); *c; ++c)
            h = (h ^ uint8_t(*c)) * 16777619u;
        return keyed::mix(h ^ l.line() * keyed::k_id2);
    }

    // The draws of one call: a key and the count of draws taken from it. A
    // value made where the rolls happen and dropped after (never stored), so
    // a function can roll several numbers, or hand it to its helpers by
    // reference. rw::random makes one; rng{k} is a stream from a fixed key
    // (the tests' inputs)
    struct rng {
        uint32_t key = 0x9E3779B9u;
        uint32_t n = 0;
        uint32_t next() noexcept { return keyed::draw(key, n++); }
        // Random.value: [0, 1]
        float value() noexcept { return float(next() >> 8) * (1.f / 16777215.f); }
        // Random.Range(int, int): [lo, hi)
        int32_t range(int32_t lo, int32_t hi) noexcept {
            return hi <= lo ? lo : lo + int32_t(next() % uint32_t(hi - lo));
        }
    };

    // The rolls of one call site, for (world seed, tick, id): what replaces a
    // stored UnityEngine.Random. `id` tells the roller apart (a dcon id's
    // .value, rw::ids of two, a creature's seed); the tick is 0 for what must
    // come out the same whenever it's rolled (a shape drawn every frame, a
    // layout made at a room's load). Several calls of the same site, the
    // same tick, for the same id roll the same numbers: a helper called in a
    // loop takes its caller's rng, or puts the loop's index in `id`
    constexpr rng random(uint32_t seed, uint32_t tick, uint32_t id, std::source_location const l = std::source_location::current()) noexcept {
        return rng{keyed::key(seed, tick, id, site(l)), 0};
    }
    // with an explicit salt (two streams of one site)
    constexpr rng random_salted(uint32_t seed, uint32_t tick, uint32_t id, uint32_t salt) noexcept {
        return rng{keyed::key(seed, tick, id, salt), 0};
    }

    // UnityEngine.Random itself (Xorshift128 seeded by InitState), for the
    // cosmetic layouts vanilla rolls from a fixed seed (SeedCob.PlaceInRoom)
    struct unity_random {
        uint32_t s[4];
        explicit unity_random(int32_t seed) noexcept {
            s[0] = uint32_t(seed);
            for (int i = 1; i < 4; ++i)
                s[i] = s[i - 1] * 1812433253u + 1u;
        }
        uint32_t next() noexcept {
            uint32_t const t = s[0] ^ (s[0] << 11);
            s[0] = s[1];
            s[1] = s[2];
            s[2] = s[3];
            s[3] = s[3] ^ (s[3] >> 19) ^ t ^ (t >> 8);
            return s[3];
        }
        float value() noexcept { return float(next() & 0x7FFFFFu) * (1.f / 8388607.f); }
        int32_t range(int32_t lo, int32_t hi) noexcept {
            return hi <= lo ? lo : lo + int32_t(next() % uint32_t(hi - lo));
        }
        // Random.Range(float, float) (RangedRandom: min * t + (1 - t) * max)
        float range(float lo, float hi) noexcept {
            float const t = value();
            return lo * t + (1.f - t) * hi;
        }
        // Random.onUnitSphere (RandomUnitVector)
        vec3 on_unit_sphere() noexcept {
            float const z = value() * 2.f - 1.f;
            float const a = value() * 2.f * mathf::pi;
            float const r = std::sqrt(mathf::max(0.f, 1.f - z * z));
            return vec3{r * std::cos(a), r * std::sin(a), z};
        }
    };
}
