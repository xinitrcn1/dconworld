#include <algorithm>
#include <cmath>
#include <vector>

#include "lizard_cosmetics.hpp"
#include "body_parts.hpp"
#include "lizard.hpp"
#include "lizard_ai.hpp"

namespace lizard_cosmetics {
    using rw::vec2;
    using rw::vec3;
    using rw::vec4;
    using world::room_view;
    using sprites::sprite;
    using lizard::breed;
    using lizard::cosmetic;
    using lizard::cosmetic_type;
    using lizard::sprites_overlap;
    using lizard_graphics::palette;
    using lizard_graphics::shared::spine_data;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;
    namespace shared = lizard_graphics::shared;

    namespace {
        // LizardScaleA0..6's sourcePixelSize.y (the B ones are the same size)
        constexpr float scale_heights[7] = {18.f, 14.f, 10.f, 18.f, 37.f, 29.f, 17.f};
        // GenericBodyPart(rad 1, surface friction 0.6, air friction 0.9): the
        // whiskers and antennae
        constexpr float feeler_rad = 1.f, feeler_surface_friction = 0.6f, feeler_air_friction = 0.9f;
        // WingScales' GenericBodyPart (rad 2, surface friction 0.5)
        constexpr float wing_rad = 2.f, wing_surface_friction = 0.5f;
        // SnowAccumulation's debris elements
        constexpr char const* snow_debris[5] = {"Cicada6head", "Cicada8head", "JellyFish0A", "mouseHeadDown", "Cicada3head"};

        char const* scale_a(int32_t graphic) {
            static char const* const names[7] = {"LizardScaleA0", "LizardScaleA1", "LizardScaleA2", "LizardScaleA3", "LizardScaleA4", "LizardScaleA5", "LizardScaleA6"};
            return names[custom::int_clamp(graphic, 0, 6)];
        }
        char const* scale_b(int32_t graphic) {
            static char const* const names[7] = {"LizardScaleB0", "LizardScaleB1", "LizardScaleB2", "LizardScaleB3", "LizardScaleB4", "LizardScaleB5", "LizardScaleB6"};
            return names[custom::int_clamp(graphic, 0, 6)];
        }
        float scale_height(int32_t graphic) {
            return scale_heights[custom::int_clamp(graphic, 0, 6)];
        }

        // Color.Lerp
        vec3 mix(vec3 a, vec3 b, float t) {
            return a + (b - a) * mathf::clamp01(t);
        }
        vec4 mix(vec4 a, vec4 b, float t) {
            t = mathf::clamp01(t);
            return vec4{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }
        vec4 opaque(vec3 c) {
            return vec4{c.x, c.y, c.z, 1.f};
        }

        // UnityEngine.Random.Range(float, float)
        float range(rw::rng& rng, float a, float b) {
            return mathf::lerp(a, b, rng.value());
        }
        // Custom.DegToVec(Random.value * 360f) * Random.value, in that order
        vec2 random_in_circle(rw::rng& rng) {
            vec2 const d = custom::deg_to_vec(rng.value() * 360.f);
            return d * rng.value();
        }

        // BodyPart.Reset
        void reset_part(player::body_part& bp, vec2 at, rw::rng& rng) {
            bp.pos = at + custom::deg_to_vec(rng.value() * 360.f);
            bp.last_pos = bp.pos;
            bp.vel = vec2{0.f, 0.f};
        }

        vec2 chunk_pos(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_pos(lizard::chunk(state, l, i));
        }
        vec2 chunk_vel(sys::game_state const& state, dcon::creature_id l, uint32_t i) {
            return state.world.body_chunk_get_vel(lizard::chunk(state, l, i));
        }

        // ---- the constructors ----

        // `c` is a LongBodyScales (with LizardScales)
        bool is_long_body_scales(cosmetic const& c) {
            return c.type == cosmetic_type::long_shoulder_scales || c.type == cosmetic_type::long_head_scales || c.type == cosmetic_type::tail_tuft
                || c.type == cosmetic_type::axolotl_gills;
        }

        // a BodyScales.scalesPositions entry and, for LongBodyScales, its
        // LizardScale, while being rolled
        struct scale_entry {
            vec2 position{0.f, 0.f};
            lizard::lizard_scale scale;
        };

        // a Template being constructed: the cosmetic and its scalesPositions
        // (with the LongBodyScales' LizardScales), committed by AddCosmetic
        struct maker {
            sys::game_state& state;
            dcon::creature_id l;
            lizard::graphics_state& g;
            rw::rng& rng;
            breed b;
            lizard::breed_params const& p;
            cosmetic c;
            std::vector<scale_entry> scales;

            float body_and_tail() const {
                return shared::body_and_tail_length(g);
            }
            void begin(cosmetic_type t, sprites_overlap o) {
                c = cosmetic{};
                c.type = t;
                c.overlap = o;
                scales.clear();
            }
            // LizardGraphics.AddCosmetic (past the fixed pools a cosmetic or
            // some of its scales are left out; its random rolls are made all
            // the same)
            void commit() {
                auto& cs = g.cosmetics;
                if (cs.count >= lizard::max_cosmetics)
                    return;
                if (!scales.empty()) {
                    bool const long_scales = is_long_body_scales(c);
                    int32_t n = std::min(int32_t(scales.size()), lizard::max_scale_positions - cs.position_count);
                    if (long_scales)
                        n = std::min(n, lizard::max_long_scales - cs.scale_count);
                    c.count = int16_t(n);
                    c.first = int16_t(cs.position_count);
                    c.first_scale = int16_t(cs.scale_count);
                    for (int32_t i = 0; i < n; ++i) {
                        cs.positions[cs.position_count++] = scales[size_t(i)].position;
                        if (long_scales)
                            cs.scales[cs.scale_count++] = scales[size_t(i)].scale;
                    }
                }
                cs.list[cs.count++] = c;
            }
            // the first committed cosmetic matching `f`, if any
            template<typename F>
            cosmetic const* find(F&& f) const {
                for (int32_t i = 0; i < g.cosmetics.count; ++i)
                    if (f(g.cosmetics.list[i]))
                        return &g.cosmetics.list[i];
                return nullptr;
            }

            // BodyScales.GeneratePatchPattern
            void generate_patch_pattern(float start, int32_t n, float max_length, float length_exponent) {
                float const num = mathf::lerp(start + 0.1f, mathf::max(start + 0.2f, max_length), mathf::pow(rng.value(), length_exponent));
                float const k = g.body_length / body_and_tail();
                scales.assign(size_t(n), scale_entry{});
                for (auto& s : scales) {
                    vec2 const v = random_in_circle(rng);
                    s.position.y = mathf::lerp(start * k, num * k, (v.y + 1.f) / 2.f);
                    s.position.x = v.x;
                }
            }
            // BodyScales.GenerateTwoLines
            void generate_two_lines(float start, float max_length, float length_exponent, float spacing_scale) {
                float const num = mathf::lerp(start + 0.1f, mathf::max(start + 0.2f, max_length), mathf::pow(rng.value(), length_exponent));
                float const num2 = num * body_and_tail();
                float num3 = mathf::lerp(2.f, 9.f, rng.value());
                if (b == breed::blue)
                    num3 = 2.f;
                num3 *= spacing_scale;
                int32_t const num4 = std::max(3, int32_t(num2 / num3));
                scales.assign(size_t(num4 * 2), scale_entry{});
                for (int32_t i = 0; i < num4; ++i) {
                    float const t = float(i) / float(num4 - 1);
                    float const y = mathf::lerp(0.f, num, t);
                    float const x = 0.6f + 0.4f * std::sin(t * mathf::pi);
                    scales[size_t(i * 2)].position = vec2{x, y};
                    scales[size_t(i * 2 + 1)].position = vec2{-x, y};
                }
            }
            // BodyScales.GenerateSegments
            void generate_segments(float start, float max_length, float length_exponent) {
                float const num = mathf::lerp(start + 0.1f, mathf::max(start + 0.2f, max_length), mathf::pow(rng.value(), length_exponent));
                float const num2 = num * body_and_tail();
                float num3 = mathf::lerp(7.f, 14.f, rng.value());
                if (b == breed::red)
                    num3 = mathf::min(num3, 11.f) * 0.75f;
                int32_t const num4 = std::max(3, int32_t(num2 / num3));
                int32_t const num5 = rng.range(1, 4) * 2;
                scales.assign(size_t(num4 * num5), scale_entry{});
                for (int32_t i = 0; i < num4; ++i) {
                    float const t = float(i) / float(num4 - 1);
                    float const y = mathf::lerp(0.f, num, t);
                    for (int32_t j = 0; j < num5; ++j) {
                        float x = 0.6f + 0.6f * std::sin(t * mathf::pi);
                        x *= mathf::lerp(-1.f, 1.f, float(j) / float(num5 - 1));
                        scales[size_t(i * num5 + j)].position = vec2{x, y};
                    }
                }
            }
            void y_range(float& lo, float& hi) const {
                hi = 0.f;
                lo = 1.f;
                for (auto const& s : scales) {
                    hi = mathf::max(hi, s.position.y);
                    lo = mathf::min(lo, s.position.y);
                }
            }
        };

        // SpineSpikes
        void add_spine_spikes(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            breed const b = m.b;
            m.begin(cosmetic_type::spine_spikes, sprites_overlap::behind_head);
            float const num = mathf::lerp(5.f, 8.f, mathf::pow(rng.value(), 0.7f));
            c.spine_length = mathf::lerp(0.2f, 0.95f, rng.value()) * m.body_and_tail();
            c.size_range_min = mathf::lerp(0.1f, 0.5f, mathf::pow(rng.value(), 2.f));
            c.size_range_max = mathf::lerp(c.size_range_min, 1.1f, rng.value());
            if (rng.value() < 0.5f)
                c.size_range_max = 1.f;
            if (b == breed::blue) {
                c.size_range_min = mathf::min(c.size_range_min, 0.3f);
                c.size_range_max = mathf::min(c.size_range_max, 0.6f);
            } else if (b != breed::green && rng.value() < 0.7f) {
                c.size_range_min *= 0.7f;
                c.size_range_max *= 0.7f;
            } else if (b == breed::green && rng.value() < 0.7f) {
                c.size_range_min = mathf::lerp(c.size_range_min, 1.1f, 0.1f);
                c.size_range_max = mathf::lerp(c.size_range_max, 1.1f, 0.4f);
            }
            c.size_skew_exponent = mathf::lerp(0.1f, 0.9f, rng.value());
            c.count = int16_t(c.spine_length / num); // bumps
            c.scale_x = 1.f;
            int32_t graphic = rng.range(0, 5);
            if (graphic == 1)
                graphic = 0;
            if (graphic == 4)
                graphic = 3;
            else if (graphic == 3 && rng.value() < 0.5f)
                c.scale_x = -1.f;
            else if (rng.value() < 0.06666667f)
                c.scale_x = -1.f;
            if (b == breed::pink && rng.value() < 0.7f)
                graphic = 0;
            else if (b == breed::green && rng.value() < 0.5f)
                graphic = 3;
            c.graphic = int8_t(graphic);
            c.colored = int8_t(rng.range(0, 3));
            if (b == breed::pink && rng.value() < 0.5f)
                c.colored = 0;
            else if (b == breed::green && rng.value() < 0.5f)
                c.colored = 2;
            else if (b == breed::green && rng.value() < 0.5f)
                c.colored = 1;
            if (b == breed::train) {
                c.colored = 1;
                c.size_range_min = 1.f;
                c.size_range_max = 3.f;
            }
            m.commit();
        }

        // BumpHawk
        void add_bump_hawk(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            m.begin(cosmetic_type::bump_hawk, sprites_overlap::behind_head);
            c.flag = rng.value() < 0.5f; // coloredHawk
            float num;
            if (c.flag) {
                num = mathf::lerp(3.f, 8.f, mathf::pow(rng.value(), 0.7f));
                c.spine_length = mathf::lerp(0.3f, 0.7f, rng.value()) * m.body_and_tail();
                c.size_range_min = mathf::lerp(0.1f, 0.2f, rng.value());
                c.size_range_max = mathf::lerp(c.size_range_min, 0.35f, mathf::pow(rng.value(), 0.5f));
            } else {
                num = mathf::lerp(6.f, 12.f, mathf::pow(rng.value(), 0.5f));
                c.spine_length = mathf::lerp(0.3f, 0.9f, rng.value()) * m.body_and_tail();
                c.size_range_min = mathf::lerp(0.2f, 0.3f, mathf::pow(rng.value(), 0.5f));
                c.size_range_max = mathf::lerp(c.size_range_min, 0.5f, rng.value());
            }
            c.size_skew_exponent = mathf::lerp(0.1f, 0.7f, rng.value());
            c.count = int16_t(c.spine_length / num);
            m.commit();
        }

        // LongShoulderScales
        void add_long_shoulder_scales(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            breed const b = m.b;
            m.begin(cosmetic_type::long_shoulder_scales, sprites_overlap::behind_head);
            c.rigor = 0.f;
            int32_t num = 0;
            if (b != breed::pink || rng.value() < 0.33333334f)
                num = rng.range(0, 3);
            else if (b == breed::green || rng.value() < 0.5f)
                num = 2;
            switch (num) {
                case 0: {
                    int32_t const n = rng.range(4, 15);
                    m.generate_patch_pattern(0.05f, n, 0.9f, 2.f);
                    break;
                }
                case 1:
                    m.generate_two_lines(0.07f, 1.f, 1.5f, 3.f);
                    break;
                default:
                    m.generate_segments(0.1f, 0.8f, 5.f);
                    break;
            }
            // MoveScalesTowardsHead
            {
                float lo = 1.f;
                for (auto const& s : m.scales)
                    lo = mathf::min(lo, s.position.y);
                if (lo > 0.07f) {
                    lo -= 0.07f;
                    for (auto& s : m.scales)
                        s.position.y -= lo;
                }
            }
            float const n = float(m.scales.size());
            float num2 = mathf::lerp(1.f, 1.f / mathf::lerp(1.f, n, mathf::pow(rng.value(), 2.f)), 0.5f);
            float num3 = mathf::lerp(5.f, 15.f, rng.value()) * num2;
            float bb = mathf::lerp(num3, 35.f, mathf::pow(rng.value(), 0.5f)) * num2;
            if (b == breed::red) {
                if (m.scales.size() > 8) {
                    // StretchDownOnBack
                    float const stretch_to = num == 0 ? 0.3f : 0.5f;
                    float lo, hi;
                    m.y_range(lo, hi);
                    if (hi <= stretch_to)
                        for (auto& s : m.scales)
                            s.position.y = custom::lerp_map(s.position.y, lo, hi, lo, stretch_to);
                }
                num2 = mathf::max(0.5f, num2);
                num3 = mathf::max(10.f, num3) * 1.2f;
                bb = mathf::max(25.f, bb) * 1.2f;
            }
            c.colored = b == breed::green || b == breed::red || rng.value() < 0.4f;
            if (rng.value() < 0.1f)
                c.graphic = int8_t(rng.range(0, 7));
            else
                c.graphic = int8_t(rng.range(3, 6));
            if (b == breed::pink && rng.value() < 0.25f)
                c.graphic = 0;
            if (b == breed::red) {
                c.graphic = 0;
                if (rng.value() < 0.3f) {
                    c.graphic = 3;
                    c.scale_x = rng.value() < 0.5f ? -1.f : 1.f;
                }
            }
            c.graphic_height = scale_height(c.graphic);
            float lo, hi;
            m.y_range(lo, hi);
            float const pw = mathf::lerp(0.1f, 0.9f, rng.value());
            for (auto& s : m.scales) {
                float const num6 = mathf::pow(mathf::inverse_lerp(lo, hi, s.position.y), pw);
                float const t = mathf::lerp(std::sin(num6 * mathf::pi), 1.f, num6 < 0.5f ? 0.5f : 0.f);
                s.scale.length = mathf::lerp(num3, bb, t);
                s.scale.width = mathf::lerp(0.8f, 1.2f, t) * num2;
                s.scale.backwards_factor = s.position.y * 0.7f;
            }
            m.commit();
        }

        // ShortBodyScales
        void add_short_body_scales(maker& m) {
            auto& rng = m.rng;
            breed const b = m.b;
            m.begin(cosmetic_type::short_body_scales, sprites_overlap::behind_head);
            int32_t num = rng.range(0, 3);
            if (b == breed::green && rng.value() < 0.7f)
                num = 2;
            else if (b == breed::blue && rng.value() < 0.93f)
                num = 1;
            switch (num) {
                case 0: {
                    int32_t const n = rng.range(4, 15);
                    m.generate_patch_pattern(0.1f, n, 0.9f, 1.2f);
                    break;
                }
                case 1:
                    m.generate_two_lines(0.1f, 1.f, 1.5f, 1.f);
                    break;
                default:
                    m.generate_segments(0.1f, 0.9f, b == breed::pink ? 1.5f : 0.6f);
                    break;
            }
            m.commit();
        }

        // TailTuft
        void add_tail_tuft(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            breed const b = m.b;
            m.begin(cosmetic_type::tail_tuft, sprites_overlap::behind_head);
            c.rigor = 0.f;
            if (rng.value() < 0.14285715f || (rng.value() < 0.9f && b == breed::blue) || b == breed::red || b == breed::zoop) {
                if (b == breed::blue || b == breed::red)
                    m.generate_two_lines(0.f, b == breed::red ? 0.3f : 0.7f, 1.f, 3.f);
                else
                    m.generate_two_lines(0.f, 0.4f, 1.2f, 1.3f);
            } else {
                int32_t const n = rng.range(3, 7);
                m.generate_patch_pattern(0.f, n, 1.6f, 1.5f);
            }
            // MoveScalesTowardsTail
            {
                float hi = 0.f;
                for (auto const& s : m.scales)
                    hi = mathf::max(hi, s.position.y);
                for (auto& s : m.scales)
                    s.position.y += 0.9f - hi;
            }
            float num = mathf::lerp(1.f, 1.f / mathf::lerp(1.f, float(m.scales.size()), mathf::pow(rng.value(), 2.f)), 0.5f);
            if (b == breed::red || b == breed::zoop)
                num = mathf::max(num, 0.4f) * 1.1f;
            float const a = mathf::lerp(5.f, 10.f, rng.value()) * num;
            float const bb = mathf::lerp(a, 25.f, mathf::pow(rng.value(), 0.5f)) * num;
            c.colored = rng.value() < 0.8f;
            int32_t graphic = rng.range(3, 7);
            if (graphic == 3)
                graphic = 1;
            if (rng.value() < 0.033333335f)
                graphic = rng.range(0, 7);
            if (rng.value() < 0.8f || b == breed::red || b == breed::zoop) {
                if (auto const* o = m.find([](cosmetic const& x) { return is_long_body_scales(x) || x.type == cosmetic_type::spine_spikes; }))
                    graphic = o->graphic;
            }
            c.graphic = int8_t(graphic);
            c.graphic_height = scale_height(c.graphic);
            float lo, hi;
            m.y_range(lo, hi);
            float const num4 = mathf::lerp(1.f, 1.5f, rng.value());
            for (auto& s : m.scales) {
                float const t = mathf::inverse_lerp(lo, hi, s.position.y);
                s.scale.length = mathf::lerp(a, bb, t);
                s.scale.width = mathf::lerp(0.8f, 1.2f, t) * num;
                s.scale.backwards_factor = 0.3f + 0.7f * mathf::inverse_lerp(0.75f, 1.f, s.position.y);
                s.position.x *= mathf::inverse_lerp(1.05f, 0.85f, s.position.y) * num4;
            }
            m.commit();
        }

        // LongHeadScales
        void add_long_head_scales(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            breed const b = m.b;
            m.begin(cosmetic_type::long_head_scales, b == breed::blizzard ? sprites_overlap::behind_head : sprites_overlap::in_front);
            c.rigor = rng.value();
            // GenerateTwoHorns
            {
                float const y = mathf::lerp(0.f, 0.07f, rng.value());
                float const x = mathf::lerp(0.5f, 1.5f, rng.value());
                m.scales.assign(2, scale_entry{});
                m.scales[0].position = vec2{-x, y};
                m.scales[1].position = vec2{x, y};
            }
            float const num = mathf::pow(rng.value(), 0.7f) * m.p.head_size;
            c.colored = rng.value() < 0.5f && b != breed::white;
            c.graphic = int8_t(rng.range(4, 6));
            if (num < 0.5f && rng.value() < 0.5f)
                c.graphic = 6;
            else if (num > 0.8f)
                c.graphic = 5;
            if (num < 0.2f && b != breed::white)
                c.colored = 1;
            if (b == breed::black)
                c.colored = 0;
            c.graphic_height = scale_height(c.graphic);
            float const value = rng.value();
            float const num2 = mathf::pow(rng.value(), 0.85f);
            for (auto& s : m.scales) {
                s.scale.length = mathf::lerp(5.f, 35.f, num);
                s.scale.width = mathf::lerp(0.65f, 1.2f, value * num);
                s.scale.backwards_factor = num2;
            }
            m.commit();
        }

        // AxolotlGills
        void add_axolotl_gills(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            m.begin(cosmetic_type::axolotl_gills, sprites_overlap::behind_head);
            c.rigor = rng.value();
            float const num = mathf::pow(rng.value(), 0.7f) * m.p.head_size;
            c.colored = 1;
            c.graphic = int8_t(rng.range(0, 6));
            if (c.graphic == 2)
                c.graphic = int8_t(rng.range(0, 6));
            c.graphic_height = scale_height(c.graphic);
            int32_t const num2 = rng.range(2, 8);
            m.scales.assign(size_t(num2 * 2), scale_entry{});
            float const value = rng.value();
            float const num3 = mathf::lerp(0.1f, 0.9f, rng.value());
            for (int32_t i = 0; i < num2; ++i) {
                float const y = mathf::lerp(0.f, 0.07f, mathf::pow(rng.value(), 1.3f));
                float const num4 = mathf::lerp(0.5f, 1.5f, rng.value());
                float const num5 = mathf::lerp(0.2f, 1.f, mathf::pow(rng.value(), 0.5f));
                float const num6 = mathf::pow(rng.value(), 0.5f);
                for (int32_t j = 0; j < 2; ++j) {
                    auto& s = m.scales[size_t(i * 2 + j)];
                    s.position = vec2{j == 0 ? -num4 : num4, y};
                    s.scale.length = mathf::lerp(5.f, 35.f, num * num5);
                    s.scale.width = mathf::lerp(0.65f, 1.2f, value * num);
                    s.scale.backwards_factor = num3 * num6;
                }
            }
            m.commit();
        }

        // TailFin
        void add_tail_fin(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            breed const b = m.b;
            m.begin(cosmetic_type::tail_fin, sprites_overlap::behind_head);
            float const num = mathf::lerp(4.f, 7.f, mathf::pow(rng.value(), 0.7f));
            c.spine_length = custom::clamped_random_variation(0.5f, 0.17f, 0.5f, rng) * m.body_and_tail();
            if (b == breed::peach)
                c.spine_length = custom::clamped_random_variation(0.4f, 0.17f, 0.5f, rng) * m.body_and_tail();
            c.underside_size = mathf::lerp(0.3f, 0.9f, rng.value());
            c.size_range_min = mathf::lerp(0.1f, 0.3f, mathf::pow(rng.value(), 2.f));
            c.size_range_max = mathf::lerp(c.size_range_min, 0.6f, rng.value());
            c.size_skew_exponent = mathf::lerp(0.5f, 1.5f, rng.value());
            c.graphic = int8_t(rng.range(0, 6));
            if (b == breed::red) {
                c.graphic = 0;
                if (auto const* o = m.find(is_long_body_scales))
                    c.graphic = o->graphic;
                c.size_range_min *= 2.f;
                c.size_range_max *= 1.5f;
                c.spine_length = custom::clamped_random_variation(0.3f, 0.17f, 0.5f, rng) * m.body_and_tail();
            }
            c.count = int16_t(c.spine_length / num);
            c.scale_x = mathf::lerp(1.f, 2.f, rng.value());
            if (c.graphic == 3 && rng.value() < 0.5f)
                c.scale_x = -c.scale_x;
            else if (c.graphic != 0 && rng.value() < 0.06666667f)
                c.scale_x = -c.scale_x;
            c.colored = rng.value() > 0.33333334f;
            m.commit();
        }

        // TailGeckoScales
        void add_tail_gecko_scales(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            m.begin(cosmetic_type::tail_gecko_scales, sprites_overlap::behind_head);
            int32_t rows = rng.range(7, 14);
            int32_t const inner = rng.range(3, 4);
            int32_t lines = rng.range(3, inner);
            float const tail_color = m.g.ivars.tail_color;
            if (tail_color > 0.1f && rng.value() < mathf::lerp(0.7f, 0.99f, tail_color)) {
                c.flag = true; // bigScales
                if (m.find([](cosmetic const& x) { return x.type == cosmetic_type::wing_scales; }) && m.g.cosmetics.wings.scale_length > 10.f)
                    c.flag = false;
            }
            if (rng.value() < 0.5f) {
                int32_t const r = rng.range(0, 7);
                rows += rng.range(0, r);
                int32_t const ln = rng.range(0, 3);
                lines += rng.range(0, ln);
            }
            c.count = int16_t(rows);
            c.lines = int16_t(lines);
            m.commit();
        }

        // Whiskers
        void add_whiskers(maker& m) {
            auto& rng = m.rng;
            auto& wh = m.g.cosmetics.whiskers;
            m.begin(cosmetic_type::whiskers, sprites_overlap::in_front);
            wh.amount = rng.range(3, 5);
            for (int32_t i = 0; i < wh.amount; ++i) {
                for (int32_t s = 0; s < 2; ++s)
                    wh.whiskers[s][i] = player::body_part{vec2{0.f, 0.f}, vec2{0.f, 0.f}, vec2{0.f, 0.f}, feeler_rad, 1.f};
                wh.directions[i] = custom::deg_to_vec(mathf::lerp(4.f, 100.f, rng.value()));
                auto& pr = wh.props[i];
                pr[0] = custom::clamped_random_variation(0.5f, 0.4f, 0.5f, rng) * 40.f;
                pr[1] = mathf::lerp(-0.5f, 0.8f, rng.value());
                pr[2] = mathf::lerp(11.f, 720.f, mathf::pow(rng.value(), 1.5f)) / pr[0];
                pr[3] = rng.value();
                pr[4] = mathf::lerp(0.6f, 1.2f, mathf::pow(rng.value(), 1.6f));
                if (i > 0)
                    for (int32_t j = 0; j < 5; ++j)
                        if (j != 1)
                            pr[j] = mathf::lerp(pr[j], wh.props[i - 1][j], mathf::pow(rng.value(), 0.3f) * 0.6f);
            }
            m.commit();
        }

        // Antennae
        void add_antennae(maker& m) {
            auto& rng = m.rng;
            auto& an = m.g.cosmetics.antennae;
            m.begin(cosmetic_type::antennae, sprites_overlap::in_front);
            an.length = rng.value();
            an.segments = std::min(int32_t(std::floor(mathf::lerp(3.f, 8.f, mathf::pow(an.length, mathf::lerp(1.f, 6.f, an.length))))), lizard::antennae_state::max_segments);
            an.alpha = an.length * 0.9f + rng.value() * 0.1f;
            for (int32_t i = 0; i < an.segments; ++i)
                for (int32_t s = 0; s < 2; ++s)
                    an.antennae[s][i] = player::body_part{vec2{0.f, 0.f}, vec2{0.f, 0.f}, vec2{0.f, 0.f}, feeler_rad, 1.f};
            vec3 t = m.state.world.creature_get_payload(m.l).lizard.effect_color;
            t.y *= 0.5f;
            t.z *= 0.5f;
            t.x = mathf::lerp(t.x, 1.f, 0.75f);
            an.redder_tint = t;
            m.commit();
        }

        // JumpRings
        void add_jump_rings(maker& m) {
            m.begin(cosmetic_type::jump_rings, sprites_overlap::in_front);
            m.commit();
        }

        // WingScales
        void add_wing_scales(maker& m) {
            auto& rng = m.rng;
            auto& ws = m.g.cosmetics.wings;
            m.begin(cosmetic_type::wing_scales, sprites_overlap::in_front);
            ws.per_side = rng.value() < 0.2f ? 3 : 2;
            ws.graphic = rng.value() < 0.4f ? 0 : rng.range(0, 5);
            ws.graphic_length = scale_height(ws.graphic);
            ws.sturdy = rng.value();
            ws.pos_squeeze = rng.value();
            ws.scale_length = mathf::lerp(5.f, 40.f, mathf::pow(rng.value(), 0.75f + 1.25f * ws.sturdy));
            ws.front_dir = mathf::lerp(-0.1f, 0.2f, rng.value());
            ws.back_dir = mathf::lerp(mathf::max(0.f, ws.front_dir), ws.front_dir + float(ws.per_side) * 0.2f, rng.value());
            ws.air_friction = mathf::lerp(0.8f, 0.999f, ws.sturdy);
            for (auto& side : ws.scales)
                for (auto& s : side)
                    s = player::body_part{vec2{0.f, 0.f}, vec2{0.f, 0.f}, vec2{0.f, 0.f}, wing_rad, 1.f};
            m.commit();
        }

        // BodyStripes (Caramel)
        void add_body_stripes(maker& m) {
            auto& rng = m.rng;
            m.begin(cosmetic_type::body_stripes, sprites_overlap::behind_head);
            float const num2 = mathf::lerp(0.4f, 0.8f, rng.value());
            float const num3 = num2 * m.body_and_tail();
            float const num4 = mathf::lerp(5.f, 12.f, rng.value()) * 1.5f;
            int32_t const num5 = std::max(3, int32_t(num3 / num4));
            m.scales.assign(size_t(num5 * 2), scale_entry{});
            for (int32_t i = 0; i < num5; ++i) {
                float const t = float(i) / float(num5 - 1);
                float const y = mathf::lerp(0.f, num2, t);
                float const x = 0.6f + 0.4f * std::sin(t * mathf::pi);
                m.scales[size_t(i * 2)].position = vec2{x, y};
                m.scales[size_t(i * 2 + 1)].position = vec2{-x, y};
            }
            m.commit();
        }

        // SnowAccumulation (Zoop)
        void add_snow_accumulation(maker& m) {
            auto& rng = m.rng;
            auto& sn = m.g.cosmetics.snow;
            m.begin(cosmetic_type::snow_accumulation, sprites_overlap::in_front);
            for (int32_t i = 0; i < lizard::snow_state::n; ++i) {
                float const v = rng.value();
                sn.debris_name[i] = v < 0.2f ? 0 : v < 0.4f ? 1 : v < 0.6f ? 2 : v < 0.8f ? 3 : 4;
                sn.debris_rotate[i] = range(rng, -40.f, 40.f);
                sn.debris_spine_pos[i] = range(rng, 0.06f, 0.7f);
                sn.debris_alphas[i] = 0.f;
            }
            m.commit();
        }

        // SkinkStripes (Indigo)
        void add_skink_stripes(maker& m) {
            m.begin(cosmetic_type::skink_stripes, sprites_overlap::body_surface);
            m.c.count = int16_t(4 + m.p.tail_segments / 2 * 2 + 2); // segs
            m.commit();
        }

        // SkinkSpeckles (Indigo, Basilisk)
        void add_skink_speckles(maker& m) {
            auto& rng = m.rng;
            auto& sp = m.g.cosmetics.speckles;
            m.begin(cosmetic_type::skink_speckles, sprites_overlap::body_surface);
            int32_t const lo = rng.range(0, 20);
            int32_t const spots = rng.range(lo, 50);
            sp.spots = std::min(spots, lizard::speckles_state::max_spots);
            for (int32_t i = 0; i < spots; ++i) {
                float const x = std::sin(rng.value() * mathf::pi * 0.5f);
                float const y = rng.value() * 2.f - 1.f;
                float const z = rng.value() * 0.6f + 0.4f;
                if (i < sp.spots)
                    sp.spot_info[i] = vec3{x, y, z};
            }
            m.commit();
        }

        // PeachHeadStripes
        void add_peach_head_stripes(maker& m) {
            auto& wc = m.g.cosmetics.watcher;
            m.begin(cosmetic_type::peach_head_stripes, sprites_overlap::in_front);
            wc.stripes_alpha = m.rng.value();
            vec3 t = m.state.world.creature_get_payload(m.l).lizard.effect_color;
            t.y *= 0.5f;
            t.z *= 0.5f;
            t.x = mathf::lerp(t.x, 1.f, 0.75f);
            wc.stripes_redder_tint = t;
            m.commit();
        }

        // PeachBackFin
        void add_peach_back_fin(maker& m) {
            auto& rng = m.rng;
            auto& c = m.c;
            m.begin(cosmetic_type::peach_back_fin, sprites_overlap::behind_head);
            c.spine_length = 0.08f;
            c.size_range_min = mathf::lerp(0.27f, 0.35f, mathf::pow(rng.value(), 2.f));
            c.size_range_max = mathf::lerp(c.size_range_min, 0.7f, rng.value());
            c.size_skew_exponent = mathf::lerp(0.5f, 1.5f, rng.value());
            c.graphic = rng.value() < 0.5f ? 4 : 5;
            c.count = rng.value() < 0.5f ? 3 : 4;
            c.scale_x = mathf::lerp(1.5f, 2.5f, rng.value());
            c.colored = 1;
            m.commit();
        }

        // the ones without random rolls
        void add_plain(maker& m, cosmetic_type t, sprites_overlap o) {
            m.begin(t, o);
            m.commit();
        }

        // ---- shared by Update and DrawSprites ----

        // LongBodyScales/BodyStripes.GetBackPos
        spine_data back_pos(shared::spine_frame const& fr, vec2 position, bool change_depth_rotation) {
            spine_data s = shared::spine_position(fr, position.y);
            float const num = mathf::clamp(position.x + s.depth_rotation, -1.f, 1.f);
            s.outer_pos = s.pos + s.perp * num * s.rad;
            if (change_depth_rotation)
                s.depth_rotation = num;
            return s;
        }

        vec2 head_pos(lizard::graphics_state const& g, float alpha) {
            return rw::lerp(g.head.last_pos, g.head.pos, alpha);
        }
        vec2 neck_pos(lizard::graphics_state const& g, float alpha) {
            return rw::lerp(g.draw_positions[0][1], g.draw_positions[0][0], alpha);
        }
        float head_depth(lizard::graphics_state const& g, float alpha) {
            return mathf::lerp(g.last_head_depth_rotation, g.head_depth_rotation, alpha);
        }

        // Whiskers.whiskerDir and AnchorPoint (without Remix)
        vec2 whisker_dir(lizard::graphics_state const& g, int32_t side, int32_t m, float alpha) {
            auto const& wh = g.cosmetics.whiskers;
            float const num = head_depth(g, alpha);
            vec2 const v{(side == 0 ? -1.f : 1.f) * (1.f - std::fabs(num)) * wh.directions[m].x + num * wh.props[m][1], wh.directions[m].y};
            return body_parts::rotate_around_origo(rw::normalized(v), custom::aim(neck_pos(g, alpha), head_pos(g, alpha)));
        }
        vec2 whisker_anchor(lizard::graphics_state const& g, int32_t side, int32_t m, float alpha) {
            return head_pos(g, alpha) + whisker_dir(g, side, m, alpha) * 3.f * g.ivars.head_size;
        }

        // Antennae.AntennaDir and AnchorPoint
        vec2 antenna_dir(lizard::graphics_state const& g, int32_t side, float alpha) {
            float const num = head_depth(g, alpha);
            vec2 const v{(side == 0 ? -1.f : 1.f) * (1.f - std::fabs(num)) * 1.5f + num * 3.5f, -1.f};
            return body_parts::rotate_around_origo(rw::normalized(v), custom::aim(neck_pos(g, alpha), head_pos(g, alpha)));
        }
        vec2 antenna_anchor(lizard::graphics_state const& g, int32_t side, float alpha) {
            return neck_pos(g, alpha) + antenna_dir(g, side, alpha) * 3.f * g.ivars.head_size;
        }

        // WingScales' spine position for row `i`
        float wing_spine_s(lizard::wing_scales_state const& ws, int32_t i) {
            return 0.025f + (0.025f + 0.15f * float(i)) * ws.pos_squeeze;
        }

        // ---- Update ----

        // LongBodyScales.Update (LizardScale.Update included)
        void update_long_body_scales(sys::game_state& state, dcon::creature_id l, cosmetic const& c, shared::spine_frame const& fr, room_view const& room) {
            auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto& rng = state.world.creature_get_graphics_random(l);
            float const head_rot = lizard_graphics::head_rotation(state, l, 1.f);
            float const backwards_exponent = mathf::lerp(1.f, 15.f, g.show_dominance);
            for (int32_t i = 0; i < c.count; ++i) {
                vec2 const position = g.cosmetics.positions[c.first + i];
                auto& sc = g.cosmetics.scales[c.first_scale + i];
                spine_data const bp = back_pos(fr, position, true);
                vec2 a = rw::lerp(bp.dir, custom::dir_vec(bp.pos, bp.outer_pos), std::fabs(bp.depth_rotation));
                if (position.y < 0.2f)
                    a -= custom::deg_to_vec(head_rot) * mathf::pow(mathf::inverse_lerp(0.2f, 0.f, position.y), 2.f) * 2.f;
                float const backwards = backwards_exponent == 1.f ? sc.backwards_factor : mathf::pow(sc.backwards_factor, backwards_exponent);
                a = rw::normalized(rw::lerp(a, bp.dir, backwards));
                vec2 const target = bp.outer_pos + a * sc.length;
                if (!custom::dist_less(sc.pos, target, sc.length / 2.f)) {
                    vec2 const d = custom::dir_vec(sc.pos, target);
                    float const k = rw::distance(sc.pos, target) - sc.length / 2.f;
                    sc.pos += d * k;
                    sc.vel += d * k;
                }
                sc.vel += rw::clamp_magnitude(target - sc.pos, mathf::lerp(10.f, 20.f, g.show_dominance)) / mathf::lerp(5.f, 1.5f, c.rigor);
                sc.vel *= mathf::lerp(1.f, 0.8f, c.rigor);
                if (g.show_dominance > 0.f)
                    sc.vel += custom::deg_to_vec(rng.value() * 360.f) * mathf::lerp(0.f, 6.f, g.show_dominance);
                player::body_part part{sc.pos, sc.last_pos, sc.vel, 0.f, 1.f};
                body_parts::connect_to_point(part, bp.outer_pos, sc.length, true, 0.f, vec2{0.f, 0.f}, 0.f, 0.f);
                sc.pos = part.pos;
                sc.vel = part.vel;
                // LizardScale.Update
                sc.vel *= room.point_submerged(sc.pos) ? 0.5f : 0.9f;
                sc.last_pos = sc.pos;
                sc.pos += sc.vel;
            }
        }

        // Whiskers.Update
        void update_whiskers(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto& rng = state.world.creature_get_graphics_random(l);
            auto& wh = g.cosmetics.whiskers;
            vec2 const host_vel = chunk_vel(state, l, 0);
            float const light = mathf::inverse_lerp(0.f, 0.3f, g.black_lizard_light_up_head);
            for (int32_t i = 0; i < 2; ++i)
                for (int32_t j = 0; j < wh.amount; ++j) {
                    auto& w = wh.whiskers[i][j];
                    w.vel += whisker_dir(g, i, j, 1.f) * wh.props[j][2];
                    if (room.point_submerged(w.pos))
                        w.vel *= 0.8f;
                    else
                        w.vel.y -= 0.6f;
                    body_parts::generic_update(w, room, feeler_air_friction, feeler_surface_friction);
                    body_parts::connect_to_point(w, whisker_anchor(g, i, j, 1.f), wh.props[j][0], false, 0.f, host_vel, 0.f, 0.f);
                    if (!custom::dist_less(g.head.pos, w.pos, 200.f))
                        w.pos = g.head.pos;
                    auto& lu = wh.light_up[j][i];
                    lu[1] = lu[0];
                    if (lu[0] < light)
                        lu[0] = mathf::lerp(lu[0], light, 0.7f) + 0.05f;
                    else
                        lu[0] -= 0.025f;
                    lu[0] += mathf::lerp(-1.f, 1.f, rng.value()) * 0.03f * g.black_lizard_light_up_head;
                    lu[0] = mathf::clamp01(lu[0]);
                }
        }

        // Antennae.Update
        void update_antennae(sys::game_state& state, dcon::creature_id l, room_view const& room) {
            auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto& rng = state.world.creature_get_graphics_random(l);
            auto& an = g.cosmetics.antennae;
            float num = state.world.lizard_data_get_ai(creature::lizard_data(state.world, l)).comm_flicker;
            if (!lizard::consious(state, l))
                num = 0.f;
            float const num2 = mathf::lerp(10.f, 7.f, an.length);
            vec2 const host_vel = chunk_vel(state, l, 0);
            for (int32_t i = 0; i < 2; ++i)
                for (int32_t j = 0; j < an.segments; ++j) {
                    auto& a = an.antennae[i][j];
                    float num3 = float(j) / float(an.segments - 1);
                    num3 = mathf::lerp(num3, mathf::inverse_lerp(0.f, 5.f, float(j)), 0.2f);
                    a.vel += antenna_dir(g, i, 1.f) * (1.f - num3 + 0.6f * num);
                    if (room.point_submerged(a.pos))
                        a.vel *= 0.8f;
                    else
                        a.vel.y -= 0.4f * num3 * (1.f - num);
                    body_parts::generic_update(a, room, feeler_air_friction, feeler_surface_friction);
                    a.pos += custom::rnv(rng) * 3.f * num;
                    vec2 p;
                    if (j == 0) {
                        a.vel += antenna_dir(g, i, 1.f) * 5.f;
                        p = g.head.pos;
                        body_parts::connect_to_point(a, antenna_anchor(g, i, 1.f), num2, true, 0.f, host_vel, 0.f, 0.f);
                    } else {
                        p = j == 1 ? antenna_anchor(g, i, 1.f) : an.antennae[i][j - 2].pos;
                        auto& prev = an.antennae[i][j - 1];
                        vec2 const d = custom::dir_vec(a.pos, prev.pos);
                        float const k = (num2 - rw::distance(a.pos, prev.pos)) * 0.5f;
                        a.pos -= d * k;
                        a.vel -= d * k;
                        prev.pos += d * k;
                        prev.vel += d * k;
                    }
                    a.vel += custom::dir_vec(p, a.pos) * 3.f * mathf::pow(1.f - num3, 0.3f);
                    if (j > 1)
                        an.antennae[i][j - 2].vel += custom::dir_vec(a.pos, an.antennae[i][j - 2].pos) * 3.f * mathf::pow(1.f - num3, 0.3f);
                    if (!custom::dist_less(g.head.pos, a.pos, 200.f))
                        a.pos = g.head.pos;
                }
        }

        // WingScales.Update
        void update_wing_scales(sys::game_state& state, dcon::creature_id l, shared::spine_frame const& fr, room_view const& room) {
            auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
            auto& rng = state.world.creature_get_graphics_random(l);
            auto& ws = g.cosmetics.wings;
            vec2 const host_vel = chunk_vel(state, l, 1);
            for (int32_t i = 0; i < ws.per_side; ++i) {
                float const num = custom::lerp_map(float(i), 0.f, float(ws.per_side - 1), ws.front_dir, ws.back_dir);
                spine_data const sd = shared::spine_position(fr, wing_spine_s(ws, i));
                float const f = mathf::lerp(g.head_depth_rotation, sd.depth_rotation, 0.3f + 0.2f * float(i));
                for (int32_t j = 0; j < 2; ++j) {
                    float const side = j == 0 ? -1.f : 1.f;
                    vec2 const root = sd.pos + sd.perp * side * sd.rad * (1.f - std::fabs(f));
                    vec2 d = sd.perp * side * (1.f - std::fabs(f));
                    d = rw::slerp(d, sd.dir * num, std::fabs(num));
                    d = rw::slerp(d, sd.perp * mathf::sign(f), std::fabs(f) * 0.5f);
                    vec2 const goal = root + d * ws.scale_length * 1.5f;
                    auto& s = ws.scales[j][i];
                    body_parts::generic_update(s, room, ws.air_friction, wing_surface_friction);
                    body_parts::connect_to_point(s, root, ws.scale_length * (i > 1 ? 0.6f : 1.f), false, 0.f, host_vel, 0.1f + 0.2f * ws.sturdy, 0.f);
                    s.vel += (goal - s.pos) * mathf::lerp(0.1f, 0.3f, ws.sturdy);
                    s.pos += (goal - s.pos) * 0.6f * mathf::pow(ws.sturdy, 3.f);
                }
            }
            if (state.world.lizard_data_get_core(creature::lizard_data(state.world, l)).anim == lizard::animation::prepare_to_jump && lizard::consious(state, l)) {
                vec2 const fwd = custom::dir_vec(chunk_pos(state, l, 1), chunk_pos(state, l, 0));
                for (int32_t k = 0; k < 2; ++k)
                    for (int32_t i = 0; i < ws.per_side; ++i) {
                        auto& s = ws.scales[k][i];
                        vec2 const r = custom::rnv(rng);
                        float const a = rng.value();
                        float const b = rng.value();
                        s.vel += r * 3.f * a + fwd * b * 5.f;
                        vec2 const r2 = custom::rnv(rng);
                        s.pos += r2 * 3.f * rng.value();
                    }
            }
        }

        // SnowAccumulation.Update (no HypothermiaExposure here: the debris
        // never piles up, so none ever shatters off as CentipedeShells)
        void update_snow(lizard::graphics_state& g) {
            auto& sn = g.cosmetics.snow;
            for (int32_t i = 0; i < lizard::snow_state::n; ++i) {
                if (i + 1 <= sn.debris_revealed)
                    sn.debris_alphas[i] = mathf::lerp(sn.debris_alphas[i], 1.f, 0.1f);
                else if (sn.debris_alphas[i] > 0.f)
                    sn.debris_alphas[i] = 0.f;
            }
        }

        // BasiliskSlowField.Update (Lizard.BasiliskMushroomField isn't ported:
        // its intensity stays 0)
        void update_slow_field(sys::game_state& state, dcon::creature_id l) {
            auto& wc = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).cosmetics.watcher;
            wc.field_smoothed_vel = rw::lerp(wc.field_smoothed_vel, chunk_vel(state, l, 0), 0.2f);
            wc.field_last_intensity = wc.field_intensity;
            float const v = rw::magnitude(wc.field_smoothed_vel);
            wc.field_sin_counter += 1.f / mathf::lerp(50.f, 10.f, mathf::inverse_lerp(3.f, 20.f, v));
            if (wc.field_sin_counter > 1.f)
                wc.field_sin_counter -= 1.f;
            float const num = 0.1f + 0.9f * mathf::pow(mathf::inverse_lerp(0.1f, 20.f, v), 2.f);
            float in = (1.f - num) * mathf::inverse_lerp(0.1f, 20.f, v);
            in += num * (0.5f + 0.5f * std::sin(wc.field_sin_counter * mathf::pi * 2.f));
            in = mathf::pow(in, mathf::inverse_lerp(18.f, 3.f, v));
            constexpr float basilisk_mushroom_field = 0.f;
            wc.field_intensity = in * basilisk_mushroom_field * 10.f;
        }

        // BlizzardHaloShine / BlizzardBeam / BlizzardSteam.Update (the
        // BlizzardModule's beam isn't ported: HasBeam is false)
        void update_halo(lizard::graphics_state& g) {
            auto& wc = g.cosmetics.watcher;
            ++wc.halo_heat_wiwble;
            wc.halo_heat *= 0.97f + mathf::clamp01(wc.halo_heat) * 0.03f;
            wc.halo_heat = mathf::max(wc.halo_heat - 0.0019f, 0.f);
        }
        void update_beam(sys::game_state& state, dcon::creature_id l) {
            auto& wc = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l)).cosmetics.watcher;
            wc.beam_last_hit_pos = wc.beam_hit_pos;
            wc.beam_last_size = wc.beam_size;
            ++wc.beam_time;
            wc.beam_size = std::fabs(std::sin(float(wc.beam_time) * 0.44f));
            wc.beam_hit_pos = chunk_pos(state, l, 0);
        }
        void update_steam(lizard::graphics_state& g, rw::rng& rng) {
            auto& h = g.cosmetics.watcher.steam_heaters_heat;
            h[1] = h[0];
            h[2] = mathf::min(h[2] + 0.0016666667f, 1.f);
            h[0] = mathf::lerp(h[0], h[2], 0.7f);
            if (mathf::pow(rng.value(), 2.f) < 0.3f && mathf::pow(rng.value(), 2.f) < h[2] * 2.f)
                h[0] = mathf::min(h[0], (1.f - rng.value() * 0.3f) * h[2]);
        }

        // ---- DrawSprites ----

        // what every cosmetic's DrawSprites/ApplyPalette reads off the lizard
        struct draw_ctx {
            sys::game_state const& state;
            dcon::creature_id l;
            lizard::graphics_state const& g;
            breed b;
            float alpha;
            palette const& pal;
            vec3 head_color; // HeadColor(timeStacker)
            vec3 effect; // effectColor
            float depth_rotation; // lGraphics.depthRotation (not interpolated)
            float bat; // BodyAndTailLength
            shared::spine_frame fr;
            rw::rng& jitter;
            std::vector<sprites::draw_cmd>& out;

            vec3 body_color(float f) const {
                return shared::body_color(state, l, f, pal);
            }
            spine_data spine(float s) const {
                return shared::spine_position(fr, s);
            }
            void add(char const* element, vec2 pos, float rotation, vec2 scale, vec2 anchor, vec3 color) const {
                sprites::add(out, sprite{element, pos, rotation, scale, anchor, color});
            }
            void add(char const* element, vec2 pos, float rotation, vec2 scale, vec2 anchor, vec4 color) const {
                vec4 const corners[4] = {color, color, color, color};
                sprites::add(out, sprite{element, pos, rotation, scale, anchor, vec3{color.x, color.y, color.z}}, corners);
            }
        };

        // the ScaleA/ScaleB pair layout most spine cosmetics share: every A
        // sprite, then every colored B one on top
        struct scale_sprite {
            vec2 pos;
            float rotation;
            vec2 scale;
            vec3 a_color;
            vec4 b_color;
        };
        void add_scale_sprites(draw_ctx const& d, scale_sprite const* s, int32_t n, int32_t graphic, float anchor_y, bool colored) {
            for (int32_t i = 0; i < n; ++i)
                d.add(scale_a(graphic), s[i].pos, s[i].rotation, s[i].scale, vec2{0.5f, anchor_y}, s[i].a_color);
            if (colored)
                for (int32_t i = 0; i < n; ++i) {
                    if (s[i].b_color.w >= 1.f)
                        d.add(scale_b(graphic), s[i].pos, s[i].rotation, s[i].scale, vec2{0.5f, anchor_y}, vec3{s[i].b_color.x, s[i].b_color.y, s[i].b_color.z});
                    else
                        d.add(scale_b(graphic), s[i].pos, s[i].rotation, s[i].scale, vec2{0.5f, anchor_y}, s[i].b_color);
                }
        }

        // a per-cosmetic scratch for the sprite pairs
        std::vector<scale_sprite>& scratch(size_t n) {
            static std::vector<scale_sprite> v;
            v.resize(n);
            return v;
        }

        // SpineSpikes
        void draw_spine_spikes(draw_ctx const& d, cosmetic const& c) {
            int32_t const n = c.count;
            auto& s = scratch(size_t(n));
            for (int32_t i = 0; i < n; ++i) {
                float const num = mathf::inverse_lerp(0.f, float(n - 1), float(i));
                float const f = mathf::lerp(0.05f, c.spine_length / d.bat, num);
                spine_data const sd = d.spine(f);
                float const num2 = mathf::lerp(c.size_range_min, c.size_range_max, std::sin(mathf::pow(num, c.size_skew_exponent) * mathf::pi));
                s[size_t(i)].pos = sd.outer_pos;
                s[size_t(i)].rotation = custom::aim(sd.perp * -sd.depth_rotation, sd.perp * sd.depth_rotation);
                s[size_t(i)].scale = vec2{mathf::sign(d.depth_rotation) * c.scale_x * num2,
                    num2 * mathf::max(0.2f, mathf::inverse_lerp(0.f, 0.5f, std::fabs(d.depth_rotation)))};
                vec3 const body = d.body_color(f);
                s[size_t(i)].a_color = body;
                vec3 const b = c.colored == 1 ? d.effect : mix(d.effect, body, mathf::pow(num, 0.5f));
                s[size_t(i)].b_color = opaque(b);
            }
            add_scale_sprites(d, s.data(), n, c.graphic, 0.15f, c.colored > 0);
        }

        // BumpHawk
        void draw_bump_hawk(draw_ctx const& d, cosmetic const& c) {
            int32_t const n = c.count;
            for (int32_t i = 0; i < n; ++i) {
                float const num = mathf::inverse_lerp(0.f, float(n - 1), float(i));
                float const f = mathf::lerp(0.05f, c.spine_length / d.bat, num);
                spine_data const sd = d.spine(f);
                float const sc = mathf::lerp(c.size_range_min, c.size_range_max,
                    mathf::lerp(std::sin(mathf::pow(num, c.size_skew_exponent) * mathf::pi), 1.f, num < 0.5f ? 0.5f : 0.f));
                vec3 col;
                if (c.flag)
                    col = mix(d.head_color, d.body_color(f), num);
                else if (d.b == breed::white)
                    col = shared::dynamic_body_color(d.state, d.l, d.pal);
                else
                    col = d.body_color(f);
                d.add("Circle20", sd.outer_pos, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, col);
            }
        }

        // LongBodyScales (and its LongHeadScales, AxolotlGills, TailTuft,
        // LongShoulderScales)
        void draw_long_body_scales(draw_ctx const& d, cosmetic const& c) {
            int32_t const n = c.count;
            auto& s = scratch(size_t(n));
            bool const head_colored = c.type == cosmetic_type::long_head_scales || c.type == cosmetic_type::axolotl_gills;
            vec3 const white_b = d.b == breed::white ? shared::head_color(d.state, d.l, 1.f, d.pal) : d.effect;
            for (int32_t i = 0; i < n; ++i) {
                vec2 const position = d.g.cosmetics.positions[c.first + i];
                auto const& sc = d.g.cosmetics.scales[c.first_scale + i];
                spine_data const bp = back_pos(d.fr, position, true);
                vec2 const tip = rw::lerp(sc.last_pos, sc.pos, d.alpha);
                s[size_t(i)].pos = bp.outer_pos;
                s[size_t(i)].rotation = custom::aim(bp.outer_pos, tip);
                s[size_t(i)].scale = vec2{sc.width * mathf::sign(bp.depth_rotation), sc.length / c.graphic_height};
                s[size_t(i)].a_color = head_colored ? d.head_color : d.body_color(position.y);
                s[size_t(i)].b_color = opaque(head_colored ? d.effect : white_b);
            }
            add_scale_sprites(d, s.data(), n, c.graphic, 0.1f, c.colored != 0);
        }

        // ShortBodyScales
        void draw_short_body_scales(draw_ctx const& d, cosmetic const& c) {
            for (int32_t i = 0; i < c.count; ++i) {
                spine_data const bp = back_pos(d.fr, d.g.cosmetics.positions[c.first + i], true);
                d.add("pixel", bp.outer_pos, custom::aim(bp.dir, bp.dir * -1.f), vec2{2.f, 3.f}, vec2{0.5f, 0.5f}, d.head_color);
            }
        }

        // TailFin
        void draw_tail_fin(draw_ctx const& d, cosmetic const& c) {
            int32_t const n = c.count;
            auto& s = scratch(size_t(n));
            for (int32_t side = 0; side < 2; ++side) {
                for (int32_t j = 0; j < n; ++j) {
                    float const num2 = mathf::inverse_lerp(0.f, float(n - 1), float(j));
                    spine_data const sd = d.spine(mathf::lerp(1.f - c.spine_length / d.bat, 1.f, num2));
                    auto& o = s[size_t(j)];
                    o.pos = side == 0 ? sd.outer_pos : sd.pos + (sd.pos - sd.outer_pos) * 0.85f;
                    o.rotation = custom::vec_to_deg(rw::lerp(sd.perp * sd.depth_rotation, sd.dir * (side == 1 ? -1.f : 1.f), num2));
                    float const num3 = mathf::lerp(c.size_range_min, c.size_range_max, std::sin(mathf::pow(num2, c.size_skew_exponent) * mathf::pi));
                    o.scale = vec2{mathf::sign(d.depth_rotation) * c.scale_x * num3,
                        num3 * mathf::max(0.2f, mathf::inverse_lerp(0.f, 0.5f, std::fabs(d.depth_rotation))) * (side == 1 ? -c.underside_size : 1.f)};
                    // ApplyPalette's (a quirk: along the front of the body)
                    o.a_color = d.body_color(mathf::lerp(0.05f, c.spine_length / d.bat, num2));
                    o.b_color = opaque(d.effect);
                    if (side == 1)
                        o.b_color.w = mathf::pow(mathf::inverse_lerp(0.3f, 1.f, std::fabs(d.depth_rotation)), 0.2f);
                }
                add_scale_sprites(d, s.data(), n, c.graphic, 0.15f, c.colored != 0);
            }
        }

        // TailGeckoScales
        void draw_tail_gecko_scales(draw_ctx const& d, cosmetic const& c) {
            int32_t const rows = c.count, lines = c.lines;
            float const depth = mathf::lerp(d.g.last_depth_rotation, d.g.depth_rotation, d.alpha);
            auto const wrap = [&](int32_t i, int32_t j) {
                float v = (float(j) + (i % 2 == 0 ? 0.5f : 0.f)) / float(lines - 1);
                v = -1.f + 2.f * v + depth;
                if (v < -1.f)
                    v += 2.f;
                else if (v > 1.f)
                    v -= 2.f;
                return v;
            };
            if (c.flag) {
                float const tail_color = d.g.ivars.tail_color;
                spine_data sd = d.spine(0.4f);
                for (int32_t i = 0; i < rows; ++i) {
                    float const num = mathf::inverse_lerp(0.f, float(rows - 1), float(i));
                    float const num2 = mathf::lerp(0.5f, 0.99f, mathf::pow(num, 0.8f));
                    spine_data const sd2 = d.spine(num2);
                    vec3 const a = d.body_color(num2);
                    for (int32_t j = 0; j < lines; ++j) {
                        float const num3 = wrap(i, j);
                        vec2 const v = sd.pos + sd.perp * (sd.rad + 0.5f) * num3;
                        vec2 const v2 = sd2.pos + sd2.perp * (sd2.rad + 0.5f) * num3;
                        vec3 col;
                        if (tail_color > 0.f) {
                            float num4 = mathf::inverse_lerp(0.5f, 1.f, std::fabs(rw::dot(custom::dir_vec(v2, v), custom::deg_to_vec(-45.f + 120.f * num3))));
                            num4 = custom::lerp_map(std::fabs(num3), 0.5f, 1.f, 0.3f, 0.f)
                                + 0.7f * mathf::pow(num4 * mathf::pow(tail_color, 0.3f), mathf::lerp(2.f, 0.5f, num));
                            if (num < 0.5f)
                                num4 *= custom::lerp_map(num, 0.f, 0.5f, 0.2f, 1.f);
                            num4 = mathf::pow(num4, mathf::lerp(2.f, 0.5f, num));
                            col = num4 < 0.5f ? mix(a, d.effect, mathf::inverse_lerp(0.f, 0.5f, num4))
                                               : mix(d.effect, vec3{1.f, 1.f, 1.f}, mathf::inverse_lerp(0.5f, 1.f, num4));
                        } else {
                            col = mix(a, d.effect, custom::lerp_map(num, 0.f, 0.8f, 0.2f, custom::lerp_map(std::fabs(num3), 0.5f, 1.f, 0.8f, 0.4f), 0.8f));
                        }
                        vec2 const scale{custom::lerp_map(std::fabs(num3), 0.4f, 1.f, sd2.rad * 3.5f / float(rows), 0.f) / 10.f, rw::distance(v, v2) * 1.1f / 20.f};
                        d.add("Circle20", (v + v2) * 0.5f, custom::aim(v, v2), scale, vec2{0.5f, 0.5f}, col);
                    }
                    sd = sd2;
                }
                return;
            }
            for (int32_t k = 0; k < rows; ++k) {
                float const f = mathf::inverse_lerp(0.f, float(rows - 1), float(k));
                float const num5 = mathf::lerp(0.4f, 0.95f, mathf::pow(f, 0.8f));
                spine_data const sd = d.spine(num5);
                vec3 const col = mix(d.body_color(num5), d.effect, 0.2f + 0.8f * mathf::pow(f, 0.5f));
                for (int32_t l = 0; l < lines; ++l) {
                    float num6 = wrap(k, l);
                    num6 = mathf::sign(num6) * mathf::pow(std::fabs(num6), 0.6f);
                    vec2 const v = sd.pos + sd.perp * (sd.rad + 0.5f) * num6;
                    d.add("tinyStar", v, custom::vec_to_deg(sd.dir), vec2{custom::lerp_map(std::fabs(num6), 0.4f, 1.f, 1.f, 0.f), 1.f}, vec2{0.5f, 0.5f}, col);
                }
            }
        }

        // Whiskers: a pointed long mesh of four segments each
        void draw_whiskers(draw_ctx const& d) {
            auto const& g = d.g;
            auto const& wh = g.cosmetics.whiskers;
            vec2 const b = custom::deg_to_vec(lizard_graphics::head_rotation(d.state, d.l, d.alpha));
            for (int32_t i = 0; i < wh.amount; ++i)
                for (int32_t j = 0; j < 2; ++j) {
                    vec2 verts[15];
                    vec4 colors[15];
                    vec2 const tip = rw::lerp(wh.whiskers[j][i].last_pos, wh.whiskers[j][i].pos, d.alpha);
                    vec2 const anchor = whisker_anchor(g, j, i, d.alpha);
                    vec2 const a = rw::normalized(whisker_dir(g, j, i, d.alpha) + b);
                    vec2 v3 = anchor;
                    float num = wh.props[i][4];
                    float num2 = 1.f;
                    float const light = mathf::lerp(wh.light_up[i][j][1], wh.light_up[i][j][0], d.alpha);
                    for (int32_t k = 0; k < 4; ++k) {
                        vec2 v4;
                        if (k < 3)
                            v4 = rw::lerp(anchor, tip, float(k + 1) / 4.f) + a * num2 * wh.props[i][0] * 0.2f;
                        else
                            v4 = tip;
                        num2 *= 0.7f;
                        vec2 const n = rw::normalized(v4 - v3);
                        vec2 const a2 = custom::perpendicular_vector(n);
                        float const dd = rw::distance(v4, v3) / (k == 0 ? 1.f : 5.f);
                        float const num3 = custom::lerp_map(float(k), 0.f, 3.f, wh.props[i][4], 0.5f);
                        vec4 const col = opaque(mix(d.head_color, vec3{1.f, 1.f, 1.f}, float(k - 1) / 2.f * light));
                        for (int32_t v = k * 4; v < k * 4 + (k == 3 ? 3 : 4); ++v)
                            colors[v] = col;
                        verts[k * 4] = v3 - a2 * (num3 + num) * 0.5f + n * dd;
                        verts[k * 4 + 1] = v3 + a2 * (num3 + num) * 0.5f + n * dd;
                        if (k < 3) {
                            verts[k * 4 + 2] = v4 - a2 * num3 - n * dd;
                            verts[k * 4 + 3] = v4 + a2 * num3 - n * dd;
                        } else {
                            verts[k * 4 + 2] = v4 + n * 2.1f;
                        }
                        num = num3;
                        v3 = v4;
                    }
                    sprites::long_mesh(d.out, verts, colors, 15);
                }
        }

        // Antennae: per side the antenna (HeadColor to effectColor) and its
        // glowing LizardAntenna layer on top
        void draw_antennae(draw_ctx const& d) {
            auto const& g = d.g;
            auto const& an = g.cosmetics.antennae;
            float const comm = d.state.world.lizard_data_get_ai(creature::lizard_data(d.state.world, d.l)).comm_flicker;
            float flicker = mathf::pow(d.jitter.value(), 1.f - 0.5f * comm) * comm;
            if (!lizard::consious(d.state, d.l))
                flicker = 0.f;
            vec4 const glow = mix(vec4{an.redder_tint.x, an.redder_tint.y, an.redder_tint.z, an.alpha}, vec4{1.f, 1.f, 1.f, an.alpha}, flicker);
            vec3 const tip_color = mix(d.effect, d.pal.black, flicker);
            // EffectColor; the LizardAntenna shader isn't reproduced: its
            // glow fades in along the antenna (by `tip`) over plain blending
            auto const effect_color = [&](int32_t part, float tip) {
                tip = mathf::pow(mathf::inverse_lerp(0.f, 0.6f, tip), 0.5f);
                return part == 0 ? opaque(mix(d.head_color, tip_color, tip)) : vec4{glow.x, glow.y, glow.z, glow.w * tip};
            };
            int32_t const n = an.segments;
            int32_t const nv = n * 4 - 1;
            vec2 verts[2][lizard::antennae_state::max_segments * 4];
            vec4 colors[2][2][lizard::antennae_state::max_segments * 4];
            for (int32_t i = 0; i < 2; ++i) {
                vec2 v = rw::lerp(head_pos(g, d.alpha), antenna_anchor(g, i, d.alpha), 0.5f);
                float num = 1.f, num2 = 0.f;
                for (int32_t j = 0; j < n; ++j) {
                    float const num3 = float(j) / float(n - 1);
                    vec2 const v2 = rw::lerp(an.antennae[i][j].last_pos, an.antennae[i][j].pos, d.alpha);
                    vec2 const nd = rw::normalized(v2 - v);
                    vec2 const a = custom::perpendicular_vector(nd);
                    float const dd = rw::distance(v2, v) / 5.f;
                    float const num4 = mathf::lerp(3.f, 1.f, mathf::pow(num3, 0.8f));
                    verts[i][j * 4] = v - a * (num + num4) * 0.5f + nd * dd;
                    verts[i][j * 4 + 1] = v + a * (num + num4) * 0.5f + nd * dd;
                    for (int32_t k = 0; k < 2; ++k) {
                        colors[i][k][j * 4] = effect_color(k, (num3 + num2) / 2.f);
                        colors[i][k][j * 4 + 1] = effect_color(k, (num3 + num2) / 2.f);
                        colors[i][k][j * 4 + 2] = effect_color(k, num3);
                        if (j < n - 1)
                            colors[i][k][j * 4 + 3] = effect_color(k, num3);
                    }
                    if (j < n - 1) {
                        verts[i][j * 4 + 2] = v2 - a * num4 - nd * dd;
                        verts[i][j * 4 + 3] = v2 + a * num4 - nd * dd;
                    } else {
                        verts[i][j * 4 + 2] = v2;
                    }
                    num = num4;
                    v = v2;
                    num2 = num3;
                }
            }
            // Sprite(side, part) = startSprite + part * 2 + side
            for (int32_t k = 0; k < 2; ++k)
                for (int32_t i = 0; i < 2; ++i)
                    sprites::long_mesh(d.out, verts[i], colors[i][k], size_t(nv));
        }

        // JumpRings
        void draw_jump_rings(draw_ctx const& d) {
            auto const& g = d.g;
            auto const& z = d.state.world.lizard_data_get_core(creature::lizard_data(d.state.world, d.l));
            float const bd = mathf::lerp(g.last_depth_rotation, g.depth_rotation, d.alpha);
            float const hd = head_depth(g, d.alpha);
            vec3 color = d.head_color;
            float num = 1.f;
            if (z.anim == lizard::animation::prepare_to_jump) {
                num = 0.5f + 0.5f * mathf::inverse_lerp(float(z.time_to_remain_in_animation), 0.f, float(z.time_in_animation));
                color = mix(d.head_color, mix(vec3{1.f, 1.f, 1.f}, d.effect, num), d.jitter.value());
            }
            sprite rings[2][2][2]; // [part][side][ring]
            for (int32_t i = 0; i < 2; ++i) {
                spine_data const sd = d.spine(0.06f + 0.12f * float(i));
                vec2 dir = sd.dir;
                if (i == 0)
                    dir = rw::normalized(dir - custom::dir_vec(neck_pos(g, d.alpha), head_pos(g, d.alpha)));
                vec2 const a2 = custom::perpendicular_vector(dir);
                float const num2 = 50.f * mathf::lerp(hd, bd, i == 0 ? 0.25f : 0.5f);
                for (int32_t j = 0; j < 2; ++j) {
                    vec2 const v2 = custom::deg_to_vec(num2 + (j == 0 ? -40.f : 40.f));
                    vec2 v3 = sd.pos + a2 * sd.rad * v2.x;
                    vec2 v4 = dir;
                    if (i == 0)
                        v4 = rw::normalized(v4 - custom::dir_vec(v3, head_pos(g, d.alpha)) * 2.f * std::fabs(v2.y));
                    else
                        v4 = rw::normalized(v4 + custom::dir_vec(v3, rw::lerp(g.tail[0].bp.last_pos, g.tail[0].bp.pos, d.alpha)) * 2.f * std::fabs(v2.y));
                    float const t = mathf::pow(mathf::clamp01(std::fabs(v2.x)), 2.f);
                    rings[0][j][i] = sprite{"Circle20", v3, custom::vec_to_deg(v4), vec2{v2.y > 0.f ? mathf::lerp(0.45f, 0.f, t) : 0.f, 0.55f}, vec2{0.5f, 0.5f}, color};
                    v3 = sd.pos + a2 * (sd.rad + 2.f * mathf::pow(mathf::clamp01(std::fabs(v2.x) * std::fabs(v2.y)), 0.5f)) * v2.x;
                    v3 -= v4 * (1.f - num) * 4.f;
                    rings[1][j][i] = sprite{"Circle20", v3, custom::vec_to_deg(v4), vec2{v2.y > 0.f ? mathf::lerp(0.27f, 0.f, t) * num : 0.f, 0.33f * num},
                        vec2{0.5f, 0.5f}, d.pal.black};
                }
            }
            for (auto const& part : rings)
                for (auto const& side : part)
                    for (auto const& r : side)
                        sprites::add(d.out, r);
        }

        // WingScales
        void draw_wing_scales(draw_ctx const& d) {
            auto const& ws = d.g.cosmetics.wings;
            vec3 const col = d.b == breed::zoop ? d.body_color(0.f) : d.pal.black;
            sprite s[2][3];
            for (int32_t i = 0; i < ws.per_side; ++i) {
                spine_data const sd = d.spine(wing_spine_s(ws, i));
                for (int32_t j = 0; j < 2; ++j) {
                    vec2 const root = sd.pos + sd.perp * (j == 0 ? -1.f : 1.f) * sd.rad * (1.f - std::fabs(sd.depth_rotation));
                    vec2 const tip = rw::lerp(ws.scales[j][i].last_pos, ws.scales[j][i].pos, d.alpha);
                    s[j][i] = sprite{scale_a(ws.graphic), root, custom::aim(root, tip), vec2{j == 0 ? -1.f : 1.f, rw::distance(root, tip) / ws.graphic_length},
                        vec2{0.5f, 0.f}, col};
                }
            }
            for (int32_t j = 0; j < 2; ++j)
                for (int32_t i = 0; i < ws.per_side; ++i)
                    sprites::add(d.out, s[j][i]);
        }

        // BodyStripes: a short tapered quad from each scale position halfway
        // to its pair's (startOfExtraSprites is even and BodyStripes comes
        // first, so an even index pairs with the next one)
        void draw_body_stripes(draw_ctx const& d, cosmetic const& c) {
            // BodyColor(timeStacker): vanilla passes the time stacker as `f`
            vec4 const body = opaque(d.body_color(d.alpha));
            vec4 const effect = opaque(d.effect);
            for (int32_t i = 0; i < c.count; ++i) {
                int32_t const pair = i % 2 == 0 ? i + 1 : i - 1;
                spine_data const bp = back_pos(d.fr, d.g.cosmetics.positions[c.first + i], true);
                spine_data const bp2 = back_pos(d.fr, d.g.cosmetics.positions[c.first + std::min(pair, int32_t(c.count) - 1)], true);
                float const dist = rw::distance(bp.outer_pos, bp2.outer_pos);
                vec2 const dir = custom::deg_to_vec(custom::aim(bp.outer_pos, bp2.outer_pos));
                vec2 const a = custom::perpendicular_vector(dir);
                vec2 const a2 = bp.outer_pos;
                vec2 const a3 = a2 + dir * (dist * 0.5f);
                vec2 const verts[4] = {a2 - a * 0.5f, a2 + a * 0.5f, a3 - a * 3.f, a3 + a * 3.f};
                vec4 const colors[4] = {body, body, effect, effect};
                sprites::long_mesh(d.out, verts, colors, 4);
            }
        }

        // SnowAccumulation
        void draw_snow(draw_ctx const& d) {
            auto const& g = d.g;
            auto const& sn = g.cosmetics.snow;
            float const bd = mathf::lerp(g.last_depth_rotation, g.depth_rotation, d.alpha);
            float const hd = head_depth(g, d.alpha);
            float const saturation = float(sn.debris_revealed) / float(lizard::snow_state::n);
            vec3 const col = mix(vec3{1.f, 1.f, 1.f}, mix(vec3{1.f, 1.f, 1.f}, shared::white_camo_color(d.state, d.l, d.pal), 0.3f), mathf::min(1.f, saturation * 1.5f));
            for (int32_t i = 0; i < lizard::snow_state::n; ++i) {
                if (sn.debris_alphas[i] <= 0.f)
                    continue;
                float const num = sn.debris_spine_pos[i];
                spine_data const sd = d.spine(num);
                vec2 dir = sd.dir;
                if (num < 0.34f)
                    dir = rw::normalized(dir - custom::dir_vec(neck_pos(g, d.alpha), head_pos(g, d.alpha)));
                vec2 const a2 = custom::perpendicular_vector(dir);
                vec2 const v2 = custom::deg_to_vec(50.f * mathf::lerp(hd, bd, 0.2f + num) + sn.debris_rotate[i]);
                float const num2 = mathf::lerp(0.9f, 0.3f, num);
                float const k = mathf::lerp(1.8f, 0.7f, num);
                vec2 const v3 = sd.pos + a2 * sd.rad * k * v2.x;
                vec2 v4 = dir;
                if (num < 0.34f)
                    v4 = rw::normalized(v4 - custom::dir_vec(v3, head_pos(g, d.alpha)) * 2.f * std::fabs(v2.y));
                else
                    v4 = rw::normalized(v4 + custom::dir_vec(v3, rw::lerp(g.tail[0].bp.last_pos, g.tail[0].bp.pos, d.alpha)) * 2.f * std::fabs(v2.y));
                float const t = mathf::pow(mathf::clamp01(std::fabs(v2.x)), 2.f);
                d.add(snow_debris[sn.debris_name[i]], v3, custom::vec_to_deg(v4), vec2{v2.y > 0.f ? mathf::lerp(num2 * 0.9f, 0.f, t) : 0.f, num2 * 1.1f},
                    vec2{0.5f, 0.5f}, vec4{col.x, col.y, col.z, sn.debris_alphas[i]});
            }
        }

        // SkinkStripes: the body and tail again as one long mesh (in the
        // tail's colours), and the stripes over it. The SkinkStripes shader
        // isn't reproduced: two lengthwise stripes in its colour stand in
        void draw_skink_stripes(draw_ctx const& d, cosmetic const& c) {
            // segs = 4 + tail.Length / 2 * 2 + 2
            constexpr int32_t max_segs = 6 + lizard::max_tail;
            int32_t const segs = std::min(int32_t(c.count), max_segs);
            size_t const nv = size_t(segs * 2);
            vec2 verts[max_segs * 2];
            vec4 colors[max_segs * 2];
            spine_data sd[max_segs];
            for (int32_t i = 0; i < segs; ++i) {
                sd[i] = d.spine(mathf::inverse_lerp(0.f, float(segs), float(i)));
                verts[size_t(i * 2)] = sd[i].pos - sd[i].perp * sd[i].rad;
                verts[size_t(i * 2 + 1)] = sd[i].pos + sd[i].perp * sd[i].rad;
            }
            if (d.g.ivars.tail_color > 0.f) {
                for (size_t i = 0; i < nv; ++i)
                    colors[i] = opaque(d.body_color(float(i / 2) * 2.f / float(nv - 1)));
                sprites::long_mesh(d.out, verts, colors, nv);
            } else {
                sprites::long_mesh(d.out, verts, nv, d.pal.black);
            }
            vec3 const color = mix(d.effect, d.pal.black, 0.5f);
            for (float const side : {-0.45f, 0.45f}) {
                for (int32_t i = 0; i < segs; ++i) {
                    auto const& s = sd[i];
                    float const across = mathf::clamp(side + s.depth_rotation * 0.5f, -1.f, 1.f);
                    float const w = s.rad * 0.18f * (1.f - std::fabs(across));
                    vec2 const mid = s.pos + s.perp * s.rad * across;
                    verts[size_t(i * 2)] = mid - s.perp * w;
                    verts[size_t(i * 2 + 1)] = mid + s.perp * w;
                    colors[size_t(i * 2)] = colors[size_t(i * 2 + 1)] = vec4{color.x, color.y, color.z, 0.5f + s.depth_rotation * 0.5f};
                }
                sprites::long_mesh(d.out, verts, colors, nv);
            }
        }

        // SkinkSpeckles
        void draw_skink_speckles(draw_ctx const& d) {
            auto const& sp = d.g.cosmetics.speckles;
            for (int32_t i = 0; i < sp.spots; ++i) {
                vec3 const info = sp.spot_info[i];
                spine_data const sd = d.spine(info.x);
                float num = sd.depth_rotation + info.y;
                if (num < -1.f)
                    num += 2.f;
                else if (num > 1.f)
                    num -= 2.f;
                float const sc = info.z * (1.f - std::fabs(num) * 0.3f) * sd.rad * 0.03f;
                d.add("Circle20", sd.pos + sd.perp * num * sd.rad * 0.9f, 0.f, vec2{sc, sc}, vec2{0.5f, 0.5f}, d.body_color(info.x));
            }
        }

        // PeachHeadStripes / BlizzardHaloShine: an element over the LizardHead
        // sprite in the head's angle
        void draw_over_head(draw_ctx const& d, sprite const& head, char const* base, vec4 color) {
            static std::string names[2][4];
            int32_t const which = base[0] == 'P' ? 0 : 1;
            if (names[which][0].empty())
                for (int32_t i = 0; i < 4; ++i)
                    names[which][i] = std::string{base} + std::to_string(i);
            int32_t const angle = custom::int_clamp(3 - int32_t(std::fabs(head_depth(d.g, d.alpha)) * 3.9f), 0, 3);
            d.add(names[which][angle].c_str(), head.pos, head.rotation, head.scale, head.anchor, color);
        }
        void draw_peach_head_stripes(draw_ctx const& d, sprite const& head) {
            auto const& wc = d.g.cosmetics.watcher;
            float const comm = d.state.world.lizard_data_get_ai(creature::lizard_data(d.state.world, d.l)).comm_flicker;
            float num = mathf::pow(d.jitter.value(), 1.f - 0.5f * comm) * comm;
            if (!lizard::consious(d.state, d.l))
                num = 0.f;
            // (the flash going dark under water, num > 0.4, needs the Yellow
            // AI's commFlicker, which a Peach lizard never gets)
            vec4 const c = mix(vec4{wc.stripes_redder_tint.x, wc.stripes_redder_tint.y, wc.stripes_redder_tint.z, wc.stripes_alpha},
                vec4{1.f, 1.f, 1.f, wc.stripes_alpha}, num);
            draw_over_head(d, head, "PeachLizardStripes", c);
        }
        void draw_halo_shine(draw_ctx const& d, sprite const& head) {
            auto const& wc = d.g.cosmetics.watcher;
            vec3 const hot = mix(vec3{0.9f, 0.05f, 0.f}, mix(vec3{1.f, 0.4f, 0.f}, vec3{1.f, 1.f, 1.f}, std::fabs(std::sin(float(wc.halo_heat_wiwble) * 0.07f)) * 0.3f), wc.halo_heat - 1.f);
            draw_over_head(d, head, "BlizardHaloShine", opaque(mix(vec3{1.f, 1.f, 1.f}, hot, wc.halo_heat)));
        }

        // PeachBackFin
        void draw_peach_back_fin(draw_ctx const& d, cosmetic const& c) {
            int32_t const n = c.count;
            auto& s = scratch(size_t(n));
            for (int32_t i = 0; i < n; ++i) {
                float const num = mathf::inverse_lerp(0.f, float(n - 1), float(i));
                spine_data const sd = d.spine(0.077f + c.spine_length * num);
                float const num2 = mathf::lerp(c.size_range_min, c.size_range_max, std::sin(mathf::pow(num, c.size_skew_exponent) * mathf::pi));
                s[size_t(i)].pos = sd.outer_pos;
                s[size_t(i)].rotation = custom::vec_to_deg(rw::lerp(sd.perp * sd.depth_rotation, sd.dir, num));
                s[size_t(i)].scale = vec2{mathf::sign(d.depth_rotation) * c.scale_x * num2,
                    num2 * mathf::max(0.2f, mathf::inverse_lerp(0.f, 0.5f, std::fabs(d.depth_rotation)))};
                s[size_t(i)].a_color = d.pal.black;
                s[size_t(i)].b_color = opaque(d.effect);
            }
            add_scale_sprites(d, s.data(), n, c.graphic, 0.15f, c.colored != 0);
        }
    }

    void add_cosmetics(sys::game_state& state, dcon::creature_id l) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto& rng = state.world.creature_get_graphics_random(l);
        breed const b = lizard::breed_of(state, l);
        maker m{state, l, g, rng, b, lizard::params_of(state, l), cosmetic{}, {}};
        g.cosmetics.count = 0;
        g.cosmetics.position_count = 0;
        g.cosmetics.scale_count = 0;
        bool const caramel = b == breed::caramel;
        if (b == breed::eel) {
            add_axolotl_gills(m);
            add_tail_gecko_scales(m);
            if (rng.value() < 0.75f) {
                add_long_shoulder_scales(m);
                add_tail_fin(m);
            } else {
                add_short_body_scales(m);
                if (rng.value() < 0.75f)
                    add_tail_fin(m);
                else
                    add_tail_tuft(m);
            }
        } else if (b == breed::zoop) {
            if (rng.value() < 0.175f)
                add_wing_scales(m);
            else
                add_spine_spikes(m);
            add_tail_tuft(m);
        }
        if (b == breed::cyan) {
            if (rng.value() < 0.75f)
                add_wing_scales(m);
            if (rng.value() < 0.5f && g.ivars.tail_color == 0.f)
                add_tail_tuft(m);
            else
                add_tail_gecko_scales(m);
            add_jump_rings(m);
        } else if (b != breed::white) {
            int32_t num8 = 0;
            bool flag = false, flag2 = false;
            if (b == breed::indigo) {
                add_skink_stripes(m);
                add_skink_speckles(m);
            } else if (caramel && rng.value() < 0.6f) {
                add_body_stripes(m);
                ++num8;
            } else if (rng.value() < 0.06666667f || (rng.value() < 0.8f && b == breed::green) || (rng.value() < 0.7f && b == breed::black)) {
                add_spine_spikes(m);
                ++num8;
            } else if (rng.value() < 0.033333335f && !caramel) {
                add_bump_hawk(m);
                ++num8;
            } else if ((rng.value() < 0.04761905f || (b == breed::pink && rng.value() < 0.5f) || (b == breed::red && rng.value() < 0.9f)) && b != breed::salamander) {
                add_long_shoulder_scales(m);
                flag = true;
                ++num8;
            } else if ((rng.value() < 0.0625f || (b == breed::blue && rng.value() < 0.5f)) && b != breed::salamander) {
                add_short_body_scales(m);
                flag2 = true;
                ++num8;
            } else if (b == breed::green && rng.value() < 0.5f) {
                add_short_body_scales(m);
                flag2 = true;
                ++num8;
            }
            if (b != breed::salamander && b != breed::indigo) {
                if (caramel && rng.value() < 0.5f) {
                    add_tail_tuft(m);
                } else if (rng.value() < 0.11111111f || (num8 == 0 && rng.value() < 0.7f) || (b == breed::pink && rng.value() < 0.6f)
                || (b == breed::blue && rng.value() < 0.96f)) {
                    add_tail_tuft(m);
                } else if (num8 < 2 && b == breed::green && rng.value() < 0.7f) {
                    if (rng.value() < 0.5f || flag || flag2) {
                        add_tail_tuft(m);
                    } else {
                        add_long_shoulder_scales(m);
                        flag = true;
                        ++num8;
                    }
                }
            }
            if (rng.value() < (num8 == 0 ? 0.7f : 0.1f) && b != breed::salamander && b != breed::yellow && b != breed::indigo
            && ((!flag && rng.value() < 0.9f) || rng.value() < 0.033333335f))
                add_long_head_scales(m);
            if (b == breed::salamander) {
                add_axolotl_gills(m);
                add_tail_fin(m);
            } else if (b == breed::black) {
                add_whiskers(m);
            } else if (b == breed::yellow) {
                add_antennae(m);
                if (num8 == 0 && rng.value() < 0.6f) {
                    add_short_body_scales(m);
                    ++num8;
                }
            } else if (b == breed::red || b == breed::train) {
                add_long_shoulder_scales(m);
                ++num8;
                add_spine_spikes(m);
                ++num8;
                if (rng.value() < 0.5f)
                    add_tail_fin(m);
                else
                    add_tail_tuft(m);
            } else if (b == breed::basilisk) {
                add_bump_hawk(m);
                add_tail_gecko_scales(m);
                add_tail_gecko_scales(m);
                for (int32_t i = 0; i < 9; ++i) {
                    add_short_body_scales(m);
                    add_tail_tuft(m);
                }
                add_long_head_scales(m);
                add_plain(m, cosmetic_type::basilisk_slow_field, sprites_overlap::hud);
                add_skink_speckles(m);
            } else if (b == breed::blizzard) {
                add_long_head_scales(m);
                for (int32_t i = 0; i < 8; ++i)
                    if (rng.value() < 0.5f)
                        add_short_body_scales(m);
                for (int32_t i = 0; i < 5; ++i) {
                    if (rng.value() < 0.5f)
                        add_axolotl_gills(m);
                    if (rng.value() < 0.8f)
                        add_spine_spikes(m);
                }
                for (int32_t i = 0; i < 10; ++i)
                    add_tail_tuft(m);
                add_plain(m, cosmetic_type::blizzard_halo_shine, sprites_overlap::in_front);
                add_plain(m, cosmetic_type::blizzard_beam, sprites_overlap::behind);
                add_plain(m, cosmetic_type::blizzard_steam, sprites_overlap::hud);
            } else if (b == breed::peach) {
                add_peach_head_stripes(m);
                add_tail_fin(m);
                add_peach_back_fin(m);
            }
            if (num8 == 0 && caramel) {
                add_bump_hawk(m);
                ++num8;
            }
        } else {
            if (rng.value() < 0.4f)
                add_bump_hawk(m);
            else if (rng.value() < 0.4f)
                add_short_body_scales(m);
            else if (rng.value() < 0.2f)
                add_long_shoulder_scales(m);
            else if (rng.value() < 0.2f)
                add_long_head_scales(m);
            if (rng.value() < 0.5f)
                add_tail_tuft(m);
        }
        if (b == breed::zoop)
            add_snow_accumulation(m);
        // (LizardRotGraphics: no rot module on the lizards here)
    }

    void reset(sys::game_state& state, dcon::creature_id l) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto& rng = state.world.creature_get_graphics_random(l);
        auto& cs = g.cosmetics;
        for (int32_t k = 0; k < cs.count; ++k) {
            switch (cs.list[k].type) {
                case cosmetic_type::whiskers:
                    for (int32_t i = 0; i < 2; ++i)
                        for (int32_t j = 0; j < cs.whiskers.amount; ++j)
                            reset_part(cs.whiskers.whiskers[i][j], whisker_anchor(g, i, j, 1.f), rng);
                    break;
                case cosmetic_type::antennae:
                    for (int32_t i = 0; i < 2; ++i)
                        for (int32_t j = 0; j < cs.antennae.segments; ++j)
                            reset_part(cs.antennae.antennae[i][j], antenna_anchor(g, i, 1.f), rng);
                    break;
                case cosmetic_type::wing_scales:
                    for (auto& side : cs.wings.scales)
                        for (auto& s : side) {
                            s.pos = s.last_pos = chunk_pos(state, l, 1);
                            s.vel = vec2{0.f, 0.f};
                        }
                    break;
                default:
                    break;
            }
        }
    }

    void update(sys::game_state& state, dcon::creature_id l, room_view const& room) {
        auto& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto const& cs = g.cosmetics;
        if (cs.count == 0)
            return;
        // SpinePosition(.., 1f) reads the body, which the cosmetics don't move
        shared::spine_frame const fr = shared::make_spine_frame(state, l, 1.f);
        for (int32_t k = 0; k < cs.count; ++k) {
            cosmetic const& c = cs.list[k];
            switch (c.type) {
                case cosmetic_type::long_shoulder_scales:
                case cosmetic_type::long_head_scales:
                case cosmetic_type::tail_tuft:
                case cosmetic_type::axolotl_gills:
                    update_long_body_scales(state, l, c, fr, room);
                    break;
                case cosmetic_type::whiskers:
                    update_whiskers(state, l, room);
                    break;
                case cosmetic_type::antennae:
                    update_antennae(state, l, room);
                    break;
                case cosmetic_type::wing_scales:
                    update_wing_scales(state, l, fr, room);
                    break;
                case cosmetic_type::snow_accumulation:
                    update_snow(g);
                    break;
                case cosmetic_type::basilisk_slow_field:
                    update_slow_field(state, l);
                    break;
                case cosmetic_type::blizzard_halo_shine:
                    update_halo(g);
                    break;
                case cosmetic_type::blizzard_beam:
                    update_beam(state, l);
                    break;
                case cosmetic_type::blizzard_steam:
                    update_steam(g, state.world.creature_get_graphics_random(l));
                    break;
                default:
                    break;
            }
        }
    }

    void build(sys::game_state const& state, dcon::creature_id l, float alpha, palette const& pal, sprites_overlap overlap, sprite const& head, rw::rng& jitter,
        std::vector<sprites::draw_cmd>& out) {
        auto const& g = state.world.lizard_data_get_graphics(creature::lizard_data(state.world, l));
        auto const& cs = g.cosmetics;
        bool any = false;
        for (int32_t k = 0; k < cs.count && !any; ++k)
            any = cs.list[k].overlap == overlap;
        if (!any)
            return;
        draw_ctx const d{state, l, g, lizard::breed_of(state, l), alpha, pal, shared::head_color(state, l, alpha, pal), shared::effect_color(state, l, pal),
            g.depth_rotation, shared::body_and_tail_length(g), shared::make_spine_frame(state, l, alpha), jitter, out};
        for (int32_t k = 0; k < cs.count; ++k) {
            cosmetic const& c = cs.list[k];
            if (c.overlap != overlap)
                continue;
            switch (c.type) {
                case cosmetic_type::spine_spikes:
                    draw_spine_spikes(d, c);
                    break;
                case cosmetic_type::bump_hawk:
                    draw_bump_hawk(d, c);
                    break;
                case cosmetic_type::long_shoulder_scales:
                case cosmetic_type::long_head_scales:
                case cosmetic_type::tail_tuft:
                case cosmetic_type::axolotl_gills:
                    draw_long_body_scales(d, c);
                    break;
                case cosmetic_type::short_body_scales:
                    draw_short_body_scales(d, c);
                    break;
                case cosmetic_type::tail_fin:
                    draw_tail_fin(d, c);
                    break;
                case cosmetic_type::tail_gecko_scales:
                    draw_tail_gecko_scales(d, c);
                    break;
                case cosmetic_type::whiskers:
                    draw_whiskers(d);
                    break;
                case cosmetic_type::antennae:
                    draw_antennae(d);
                    break;
                case cosmetic_type::jump_rings:
                    draw_jump_rings(d);
                    break;
                case cosmetic_type::wing_scales:
                    draw_wing_scales(d);
                    break;
                case cosmetic_type::body_stripes:
                    draw_body_stripes(d, c);
                    break;
                case cosmetic_type::snow_accumulation:
                    draw_snow(d);
                    break;
                case cosmetic_type::skink_stripes:
                    draw_skink_stripes(d, c);
                    break;
                case cosmetic_type::skink_speckles:
                    draw_skink_speckles(d);
                    break;
                case cosmetic_type::peach_head_stripes:
                    draw_peach_head_stripes(d, head);
                    break;
                case cosmetic_type::blizzard_halo_shine:
                    draw_halo_shine(d, head);
                    break;
                case cosmetic_type::peach_back_fin:
                    draw_peach_back_fin(d, c);
                    break;
                // BasiliskSlowField (the Adrenaline shader, never lit here:
                // no mushroom field), BlizzardBeam (no beam) and BlizzardSteam
                // (a HeatDistortion shader) draw nothing here
                default:
                    break;
            }
        }
    }
}
