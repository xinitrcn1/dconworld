#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>

#include "gate_graphics.hpp"
#include "gates.hpp"

namespace gate_graphics {
    using rw::vec2;
    using rw::vec3;
    using world::gate_mode;
    namespace mathf = rw::mathf;
    namespace custom = rw::custom;

    namespace {
        // gates.cpp runs the whole cycle twice as fast as vanilla's (its
        // speed_up): the machinery's own timings follow, every tick of the
        // gate's logic runs twice (the doors' factor stepped half way each
        // time), and the counters vanilla reads are scaled the same way
        constexpr int32_t speed_up = 2;
        // DoorGraphic.ClampsPause
        constexpr float clamps_pause = 0.2f;
        // the room went undrawn longer than this: start the drawing afresh
        constexpr uint32_t max_catch_up = 80;
        constexpr int32_t max_steam = 400;

        // RegionGateGraphics.Clamp.Mode
        enum class clamp_mode : uint8_t {
            stacked,
            moving_down,
            waiting_for_partner,
            locked,
            moving_up,
        };

        // RegionGateGraphics.Clamp (previous, next and partner by number:
        // number - 1, number + 1 of its side; the side 1 clamp's partner is
        // side 0's of its number, side 0 has none)
        struct clamp {
            int32_t side = 0;
            int32_t number = 0;
            float vel_y = 0.f;
            vec2 pos{0.f, 0.f};
            float rotat = 0.f;
            float depth = 0.f;
            float fric = 0.85f;
            float clamp_height = 0.f;
            float stack_height = 0.f;
            int32_t lock_counter = 0;
            clamp_mode mode = clamp_mode::stacked;
        };

        // what a door's DrawSprites interpolates, at the start of the tick
        // (vanilla's [., 1] / last values are its logic's here, a tick of it
        // being two steps)
        struct door_pose {
            float tracks[3]{};
            float poles[4]{};
            float blocks[2][2]{};
            float arms[2][2]{};
            float pansar[9]{};
            float pansar_rot[9]{};
            vec2 clamp_pos[2][9]{};
            float clamp_rot[2][9]{};
            float screws[2]{};
            float pc = 0.f;
        };

        // RegionGateGraphics.DoorGraphic
        struct door_graphic {
            int32_t number = 0;
            bool flip = false;
            float last_closed_fac = 0.f;
            float last_pc = 0.f;
            float pc = 0.f;
            float locks_locked = 0.f;
            bool bolts[4]{};
            vec2 pos_z{0.f, 0.f};
            float tracks[3][3]{};
            float poles[4][3]{};
            clamp clamps[2][9]{};
            float blocks[2][2][3]{};
            float arms[2][2][3]{};
            float pansar_locks[9][4]{};
            float big_screws[2][2]{};
            int32_t clamps_status = 0;
            int32_t wanted_clamp_status = -1;
            door_pose prev;
        };

        // WaterFall (a WaterGate's: tile (18 + 9i, 23), 3 tiles wide)
        struct water_fall {
            vec2 pos{0.f, 0.f};
            float flow = 0.f;
            float last_flow = 0.f;
            float set_flow = 0.f;
            float visual_density = 0.f;
            float last_visual_dens = 0.f;
            float top[3]{};
            float bottom[3]{};
        };

        // SteamSmoke.SteamParticle (with SpriteSmoke's and
        // SmokeSystemParticle's parts); `next` its nextParticle by slot
        struct steam_particle {
            bool alive = false;
            uint32_t id = 0;
            vec2 pos{0.f, 0.f};
            vec2 last_pos{0.f, 0.f};
            vec2 vel{0.f, 0.f};
            vec2 linger{0.f, 0.f};
            vec2 last_linger{0.f, 0.f};
            float life = 1.f;
            float last_life = 1.f;
            float life_time = 60.f;
            float rad = 100.f;
            float up_force = 0.f;
            float move_dir = 0.f;
            float intensity = 1.f;
            float stretched = 0.f;
            float last_stretched = 0.f;
            float left = 0.f, bottom = 0.f, right = 0.f, top = 0.f; // confines
            int32_t next = -1;
            uint32_t next_id = 0;
        };

        struct gate_visual {
            dcon::room_id room;
            uint32_t tick = 0;
            bool electric = false;
            float strike_level = -200.f;
            door_graphic doors[3];
            // the doors' factor at the last tick (the steps between)
            float closed_prev[3]{};
            // WaterGate
            water_fall falls[2];
            float water_left = 1.f;
            float outlet_lag = 0.f;
            vec2 heater_pos[2]{};
            float heat[2][3]{};
            float heat_prev[2]{};
            vec2 heater_quads[2][2][4]{};
            float heater_light_alpha = 0.f;
            float heater_light_rad = 0.f;
            vec3 heater_light_color{1.f, 1.f, 1.f};
            vec2 heater_light_pos{0.f, 0.f};
            // ElectricGate
            float electric_steam = 0.f;
            float battery_left = 1.f;
            bool battery_changing = false;
            float meter_height = -64.f;
            bool lamps_on[4]{};
            int32_t busted_lamp = -1;
            int32_t lamp_blink = 0;
            float lamp_alpha[4][2]{};
            float lamp_rad[4][2]{};
            // SteamSmoke
            std::vector<steam_particle> steam;
            int32_t smoke_counter = 1000;
            int32_t last_emit = -1;
            uint32_t last_emit_id = 0;
            uint32_t next_id = 1;
        };

        std::unordered_map<uint32_t, gate_visual>& visuals() {
            static std::unordered_map<uint32_t, gate_visual> m;
            return m;
        }

        // Custom.RotateAroundOrigo (clockwise degrees)
        vec2 rotate_around_origo(vec2 v, float deg) {
            float const a = deg * -0.017453292f;
            float const c = std::cos(a), sn = std::sin(a);
            return vec2{c * v.x - sn * v.y, sn * v.x + c * v.y};
        }

        // Custom.CirclesCollisionTime
        float circles_collision_time(float x1, float y1, float x2, float y2, float vx1, float vy1, float r1, float r2) {
            constexpr float n = 0.001f;
            constexpr float n2 = 0.001f;
            float const n3 = -x1 * vx1 - y1 * vy1 + vx1 * x2 + vy1 * y2 + x1 * n - x2 * n + y1 * n2 - y2 * n2;
            float const n5 = vx1 * vx1 + vy1 * vy1 - 2.f * vx1 * n + n * n - 2.f * vy1 * n2 + n2 * n2;
            float const n6 = x1 * x1 + y1 * y1 - r1 * r1 - 2.f * x1 * x2 + x2 * x2 - 2.f * y1 * y2 + y2 * y2 - 2.f * r1 * r2 - r2 * r2;
            return (2.f * n3 - std::sqrt((-2.f * n3) * (-2.f * n3) - 4.f * n5 * n6)) / (2.f * n5);
        }

        bool all_doors_in_position(vec3 const& closed, vec3 const& goal) {
            return closed[0] == goal[0] && closed[1] == goal[1] && closed[2] == goal[2];
        }

        bool name_is(std::string_view a, std::string_view b) {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
                    return false;
            return true;
        }

        // ---- RegionGateGraphics.DoorGraphic

        // the DoorGraphic constructor (and Clamp's) for a door shut `closed`, then Reset
        void init_door(door_graphic& d, int32_t number, float closed, sys::game_state const& s, uint32_t gid, rw::rng& rnd) {
            d = door_graphic{};
            d.number = number;
            d.last_closed_fac = closed;
            d.pos_z = vec2{(15.f + 9.f * float(number)) * 20.f, 340.f};
            d.clamps_status = 0;
            // (vanilla's -1 + 2 * (int)closedFac: a door caught half shut
            // here wants its clamps down)
            d.wanted_clamp_status = closed > clamps_pause ? 1 : -1;
            d.locks_locked = closed;
            for (int32_t i = 0; i < 9; ++i)
                for (int32_t j = 0; j < 2; ++j) {
                    d.pansar_locks[i][j] = closed;
                    d.pansar_locks[i][j + 2] = 1.f - closed;
                }
            for (int32_t k = 0; k < 4; ++k) {
                d.bolts[k] = closed == 1.f;
                d.poles[k][0] = 1.f - closed;
                d.poles[k][1] = 1.f - closed;
            }
            for (int32_t l = 0; l < 2; ++l) {
                for (int32_t m = 0; m < 9; ++m) {
                    clamp& c = d.clamps[l][m];
                    c.side = l;
                    c.number = m;
                    c.stack_height = d.pos_z.y - 5.f;
                    c.clamp_height = d.pos_z.y - 180.f * (float(m + 1) / 9.f);
                    c.fric = mathf::lerp(0.75f, 0.95f, mathf::pow(rnd.value(), 3.f));
                    if (closed == 1.f) {
                        c.mode = clamp_mode::locked;
                        c.pos = vec2{d.pos_z.x, c.clamp_height};
                    } else {
                        c.mode = clamp_mode::stacked;
                        c.pos = vec2{d.pos_z.x, c.stack_height};
                    }
                }
                // SeededRandom(room index + door number + l)
                d.big_screws[l][0] = rw::random(s.seed, 0, rw::ids(gid, uint32_t(number + l))).value() * 360.f;
                d.big_screws[l][1] = d.big_screws[l][0];
            }
            if (number == 0)
                d.flip = false;
            else if (number == 2)
                d.flip = true;
            else
                d.flip = rw::random(s.seed, 0, gid).value() > 0.5f;
            // Reset
            for (int32_t i = 0; i < 3; ++i)
                d.tracks[i][2] = rnd.value() * 0.5f;
            for (int32_t j = 0; j < 4; ++j)
                d.poles[j][2] = 1.f / mathf::lerp(10.f, 60.f, mathf::pow(rnd.value(), 2.1f));
            for (int32_t k = 0; k < 2; ++k)
                for (int32_t l = 0; l < 2; ++l) {
                    d.blocks[k][l][2] = rnd.value() * 0.4f;
                    d.arms[k][l][2] = 0.2f + 0.8f * rnd.value();
                }
        }

        // Clamp.StackPos
        vec2 stack_pos(door_graphic const& d, clamp const& c) {
            int32_t num = 0;
            for (int32_t n = c.number + 1; n < 9 && d.clamps[c.side][n].mode == clamp_mode::stacked; ++n)
                ++num;
            return vec2{d.pos_z.x, c.stack_height} + vec2{(-1.f + 2.f * float(c.side)) * (7.f + 7.f * float(std::min(num, 2))), 6.f * float(num)};
        }

        // Clamp.Update (its sounds aren't played)
        void update_clamp(door_graphic& d, clamp& c, int32_t desired) {
            clamp* const next = c.number < 8 ? &d.clamps[c.side][c.number + 1] : nullptr;
            clamp* const previous = c.number > 0 ? &d.clamps[c.side][c.number - 1] : nullptr;
            clamp* const partner = c.side == 1 ? &d.clamps[0][c.number] : nullptr;
            if (c.mode == clamp_mode::locked)
                c.rotat = 0.f;
            else
                c.rotat = mathf::lerp(c.rotat,
                    (-1.f + 2.f * float(c.side)) * 45.f * mathf::inverse_lerp(c.stack_height - 40.f * (float(c.number + 1) / 9.f), c.stack_height, c.pos.y), 0.2f);
            if (desired == -1 && c.mode != clamp_mode::stacked)
                c.mode = clamp_mode::moving_up;
            else if (desired == 1 && c.mode != clamp_mode::locked && c.mode != clamp_mode::waiting_for_partner
                && (next == nullptr || next->pos.y < c.stack_height - 30.f))
                c.mode = clamp_mode::moving_down;
            if (c.mode == clamp_mode::locked)
                c.depth = 0.1f;
            if (c.mode == clamp_mode::moving_up)
                c.depth = 0.06666667f;
            else
                c.depth = mathf::lerp(c.depth, 0.f, 0.1f);
            if (next != nullptr)
                c.depth = mathf::min(c.depth, next->depth);
            if (c.mode == clamp_mode::stacked) {
                c.vel_y = 0.f;
                c.pos = rw::lerp(c.pos, stack_pos(d, c), 0.3f);
            } else if (c.mode == clamp_mode::moving_down) {
                c.pos.y += c.vel_y;
                c.vel_y *= c.fric;
                c.vel_y -= 0.5f;
                if (c.pos.y < c.clamp_height) {
                    c.pos.y = c.clamp_height;
                    c.vel_y = 0.f;
                    c.mode = clamp_mode::waiting_for_partner;
                } else if (next != nullptr && c.pos.y < next->pos.y + 20.f) {
                    next->vel_y += c.vel_y;
                    c.vel_y = 0.f;
                    c.pos.y = next->pos.y + 20.f;
                }
            } else if (c.mode == clamp_mode::waiting_for_partner) {
                if (partner != nullptr && partner->mode == clamp_mode::waiting_for_partner) {
                    if (++c.lock_counter > 10) {
                        c.mode = clamp_mode::locked;
                        partner->mode = clamp_mode::locked;
                        c.lock_counter = 0;
                    }
                }
            } else if (c.mode == clamp_mode::moving_up) {
                c.pos.y += c.vel_y;
                c.vel_y *= c.fric;
                c.vel_y = mathf::lerp(c.vel_y, 3.6f, 0.2f);
                if (c.pos.y > c.stack_height) {
                    c.mode = clamp_mode::stacked;
                } else if (previous != nullptr && previous->mode != clamp_mode::stacked && c.pos.y > previous->pos.y - 20.f) {
                    previous->vel_y += c.vel_y;
                    c.vel_y *= 0.2f;
                    c.pos.y = previous->pos.y - 20.f;
                }
            }
            if (c.mode == clamp_mode::locked || c.mode == clamp_mode::moving_up) {
                c.pos.x = d.pos_z.x;
                return;
            }
            if (c.mode != clamp_mode::stacked)
                c.pos.x = d.pos_z.x + (-1.f + 2.f * float(c.side)) * 4.f;
        }

        // DoorGraphic.Update for the door shut `closed` going to `goal` (no
        // sounds, no screen shake; movementStalledByGraphicsModule isn't fed
        // back to the simulation and PansarPush isn't done here)
        void update_door(door_graphic& d, float closed, float goal) {
            if (closed > clamps_pause && d.last_closed_fac <= clamps_pause)
                d.wanted_clamp_status = 1;
            else if (closed < clamps_pause && d.last_closed_fac >= clamps_pause)
                d.wanted_clamp_status = -1;
            d.last_closed_fac = closed;
            d.last_pc = d.pc;
            d.pc = mathf::inverse_lerp(0.55f, 0.75f, closed);
            if (closed > 0.95f)
                d.locks_locked += 0.016666668f;
            else
                d.locks_locked = 0.f;
            float const tracks_closed = mathf::inverse_lerp(0.f, 0.2f, closed);
            for (int32_t i = 0; i < 3; ++i) {
                d.tracks[i][1] = d.tracks[i][0];
                d.tracks[i][0] = mathf::inverse_lerp(i == 1 ? 1.f : 0.7f, d.tracks[i][2], tracks_closed);
            }
            for (int32_t j = 0; j < 4; ++j) {
                d.poles[j][1] = d.poles[j][0];
                if (closed != 0.f)
                    d.poles[j][0] = mathf::max(d.poles[j][0] - 0.2f, 0.f);
                else
                    d.poles[j][0] = mathf::min(d.poles[j][0] + d.poles[j][2], 1.f);
            }
            float const locks_closed = mathf::inverse_lerp(0.78f, 0.9f, closed);
            for (int32_t k = 0; k < 9; ++k) {
                d.pansar_locks[k][1] = d.pansar_locks[k][0];
                float const num3 = (float(k) + 0.5f) / 9.f;
                d.pansar_locks[k][0] = mathf::lerp(0.f, num3, mathf::pow(locks_closed, 1.5f - num3)) - mathf::pow(1.f - locks_closed, 2.f) * 0.3f;
                d.pansar_locks[k][3] = d.pansar_locks[k][2];
                if (1.f - d.locks_locked < num3)
                    d.pansar_locks[k][2] = mathf::max(d.pansar_locks[k][2] - 0.033333335f, 0.f);
                else
                    d.pansar_locks[k][2] = mathf::min(d.pansar_locks[k][2] + 0.1f, 1.f);
                if (k % 2 == 0 && k < 8) {
                    if (d.pansar_locks[k][2] == 0.f && d.pansar_locks[k][3] > 0.f)
                        d.bolts[k / 2] = true;
                    else if (d.pansar_locks[k][0] < 0.f && d.pansar_locks[k][1] >= 0.f)
                        d.bolts[k / 2] = false;
                }
            }
            d.clamps_status = -2;
            for (int32_t l = 0; l < 2; ++l)
                for (int32_t m = 0; m < 9; ++m) {
                    clamp& c = d.clamps[l][m];
                    update_clamp(d, c, d.wanted_clamp_status);
                    int32_t const st = c.mode == clamp_mode::stacked ? -1 : (c.mode == clamp_mode::locked ? 1 : 0);
                    if (d.clamps_status == -2)
                        d.clamps_status = st;
                    else if (st != d.clamps_status)
                        d.clamps_status = 0;
                }
            float const blocks_closed = mathf::inverse_lerp(0.2f, 0.5f, closed);
            float const arms_withdrawn = mathf::inverse_lerp(0.52f, 0.73f, closed);
            for (int32_t n = 0; n < 2; ++n)
                for (int32_t o = 0; o < 2; ++o) {
                    d.blocks[n][o][1] = d.blocks[n][o][0];
                    d.blocks[n][o][0] = mathf::inverse_lerp(d.blocks[n][o][2], 1.f, blocks_closed);
                    d.arms[n][o][1] = d.arms[n][o][0];
                    d.arms[n][o][0] = mathf::inverse_lerp(0.f, d.arms[n][o][2], arms_withdrawn);
                }
            for (int32_t n = 0; n < 2; ++n) {
                d.big_screws[n][1] = d.big_screws[n][0];
                float const dir = ((n == 0) == d.flip) ? -1.f : 1.f;
                if (goal == 1.f)
                    d.big_screws[n][0] += std::sin(mathf::pi * mathf::inverse_lerp(0.f, 0.6f, closed)) * 2.5f * dir;
                else
                    d.big_screws[n][0] -= mathf::pow(std::sin(mathf::pi * mathf::inverse_lerp(0.3f, 1.f, closed)), 2.f) * 3.f * dir;
            }
        }

        door_pose pose_of(door_graphic const& d) {
            door_pose p;
            for (int32_t i = 0; i < 3; ++i)
                p.tracks[i] = d.tracks[i][0];
            for (int32_t i = 0; i < 4; ++i)
                p.poles[i] = d.poles[i][0];
            for (int32_t i = 0; i < 2; ++i)
                for (int32_t j = 0; j < 2; ++j) {
                    p.blocks[i][j] = d.blocks[i][j][0];
                    p.arms[i][j] = d.arms[i][j][0];
                }
            for (int32_t k = 0; k < 9; ++k) {
                p.pansar[k] = d.pansar_locks[k][0];
                p.pansar_rot[k] = d.pansar_locks[k][2];
            }
            for (int32_t l = 0; l < 2; ++l)
                for (int32_t m = 0; m < 9; ++m) {
                    p.clamp_pos[l][m] = d.clamps[l][m].pos;
                    p.clamp_rot[l][m] = d.clamps[l][m].rotat;
                }
            p.screws[0] = d.big_screws[0][0];
            p.screws[1] = d.big_screws[1][0];
            p.pc = d.pc;
            return p;
        }

        // ---- WaterFall

        // the WaterFall constructor (flow 0)
        void init_fall(water_fall& f, int32_t i, float strike) {
            f = water_fall{};
            // room.MiddleOfTile(18 + 9i, 23) + (-10, 15)
            f.pos = vec2{float(18 + 9 * i) * 20.f + 10.f - 10.f, 23.f * 20.f + 10.f + 15.f};
            f.top[0] = f.top[1] = strike;
            f.bottom[0] = f.bottom[1] = strike;
        }

        // WaterFall.Update: the stream's ends (its drips, bubbles and push on
        // swimmers aren't done)
        void update_fall(water_fall& f, float strike) {
            float const bottom_limit = mathf::min(f.pos.y, strike);
            f.last_flow = f.flow;
            f.last_visual_dens = f.visual_density;
            if (f.top[0] == f.pos.y)
                f.visual_density = mathf::lerp(f.visual_density, f.flow, 0.1f);
            if (f.top[0] == f.pos.y || f.top[0] <= bottom_limit)
                f.flow = f.set_flow;
            f.bottom[1] = f.bottom[0];
            f.bottom[0] += f.bottom[2];
            f.bottom[2] -= 0.9f;
            if (f.bottom[0] < bottom_limit) {
                f.bottom[0] = bottom_limit;
                f.bottom[2] = 0.f;
            }
            if (f.flow == 0.f) {
                f.top[1] = f.top[0];
                f.top[0] += f.top[2];
                f.top[2] -= 0.9f;
                if (f.top[0] < bottom_limit) {
                    f.top[0] = bottom_limit;
                    f.top[2] = 0.f;
                    f.visual_density = 0.f;
                }
            } else {
                f.top[0] = f.pos.y;
                f.top[1] = f.pos.y;
                f.top[2] = 0.f;
                if (f.last_flow == 0.f) {
                    f.bottom[0] = f.pos.y;
                    f.bottom[1] = f.pos.y;
                    f.bottom[2] = 0.f;
                }
            }
        }

        // ---- SteamSmoke

        // SteamSmoke.EmitSmoke (SmokeSystem.AddParticle, SteamParticle.Reset);
        // confines (left, bottom, right, top)
        void emit_steam(gate_visual& v, vec2 pos, vec2 vel, float left, float bottom, float right, float top, float intensity, rw::rng& rnd) {
            bool const connect = v.smoke_counter < 2;
            v.smoke_counter = 0;
            int32_t alive = 0;
            int32_t slot = -1;
            for (int32_t i = 0; i < int32_t(v.steam.size()); ++i) {
                if (v.steam[size_t(i)].alive)
                    ++alive;
                else if (slot < 0)
                    slot = i;
            }
            if (slot < 0) {
                if (int32_t(v.steam.size()) >= max_steam)
                    return;
                slot = int32_t(v.steam.size());
                v.steam.emplace_back();
            }
            ++alive;
            steam_particle& p = v.steam[size_t(slot)];
            p = steam_particle{};
            p.alive = true;
            p.id = v.next_id++;
            p.vel = vel;
            p.life_time = mathf::lerp(60.f, 180.f, rnd.value() * intensity);
            p.pos = pos;
            p.last_pos = pos;
            p.linger = pos + custom::rnv(rnd);
            p.last_linger = p.linger;
            p.life = 1.f;
            p.last_life = 1.f;
            p.up_force = rnd.value() * 100.f / p.life_time;
            p.move_dir = rnd.value() * 360.f;
            p.left = left;
            p.bottom = bottom;
            p.right = right;
            p.top = top;
            p.intensity = intensity;
            p.rad = mathf::lerp(108.f, 286.f, rnd.value()) * mathf::lerp(0.5f, 1.f, intensity);
            if (connect && alive > 2 && v.last_emit >= 0 && size_t(v.last_emit) < v.steam.size() && v.steam[size_t(v.last_emit)].alive
                && v.steam[size_t(v.last_emit)].id == v.last_emit_id) {
                p.next = v.last_emit;
                p.next_id = v.last_emit_id;
            }
            v.last_emit = slot;
            v.last_emit_id = p.id;
        }

        steam_particle* next_of(gate_visual& v, steam_particle const& p) {
            if (p.next < 0 || size_t(p.next) >= v.steam.size())
                return nullptr;
            steam_particle& n = v.steam[size_t(p.next)];
            return n.alive && n.id == p.next_id ? &n : nullptr;
        }

        // SmokeSystem.Update and each particle's Update (no wind from the
        // objects moving through it, the water level isn't checked)
        void update_steam(gate_visual& v, rw::rng& rnd) {
            ++v.smoke_counter;
            for (auto& p : v.steam) {
                if (!p.alive)
                    continue;
                // SmokeSystemParticle.Update
                p.last_life = p.life;
                p.last_linger = p.linger;
                p.last_pos = p.pos;
                p.pos += p.vel;
                p.life = mathf::max(0.f, p.life - 1.f / p.life_time);
                if (p.life <= 0.f && p.last_life <= 0.f) {
                    p.alive = false;
                    continue;
                }
                // SpriteSmoke.Update
                if (steam_particle* n = next_of(v, p)) {
                    vec2 const b = rw::lerp(p.vel, n->vel, 0.5f);
                    p.vel = rw::lerp(p.vel, b, 0.4f);
                    n->vel = rw::lerp(n->vel, b, 0.7f);
                    p.linger = n->pos;
                }
                p.last_stretched = p.stretched;
                p.stretched = mathf::inverse_lerp(60.f, 200.f, rw::distance(p.pos, p.linger));
                // SteamParticle.Update
                p.move_dir += mathf::lerp(-1.f, 1.f, rnd.value()) * 50.f;
                p.vel *= 0.8f;
                p.vel += custom::deg_to_vec(p.move_dir) * 1.8f * p.intensity * p.life;
                p.vel.y += 2.8f * p.intensity * p.up_force;
                p.pos.x = mathf::clamp(p.pos.x, p.left, p.right);
                p.pos.y = mathf::clamp(p.pos.y, p.bottom, p.top);
            }
        }

        // ---- RegionGateGraphics

        void init_visual(gate_visual& v, sys::game_state const& s, world::room_view const& room, dcon::gate_id g, vec2 heater_size) {
            auto const& w = s.world;
            uint32_t const gid = uint32_t(g.index());
            v = gate_visual{};
            v.room = room.id;
            v.tick = s.tick;
            v.electric = w.gate_get_electric(g) != 0;
            // WaterFall.strikeLevel: the room's water (no Remix: 10 under its
            // surface), else 200 under the room
            v.strike_level = room.water ? room.float_water_level - 10.f : -200.f;
            vec3 const closed = w.gate_get_door_closed(g);
            rw::rng rnd = rw::random(s.seed, 0, gid);
            for (int32_t i = 0; i < 3; ++i) {
                v.closed_prev[i] = closed[i];
                init_door(v.doors[i], i, closed[i], s, gid, rnd);
            }
            if (!v.electric) {
                for (int32_t i = 0; i < 2; ++i)
                    init_fall(v.falls[i], i, v.strike_level);
                v.heater_pos[0] = vec2{390.f, 30.f};
                v.heater_pos[1] = vec2{570.f, 30.f};
                // RegionGate_Heater's size: the quads are jittered by up to
                // 2 px either way
                vec2 const size = heater_size;
                for (int32_t j = 0; j < 2; ++j) {
                    vec2 const a = v.heater_pos[j];
                    vec2 q[4] = {a + vec2{-size.x / 2.f, -size.y / 2.f}, a + vec2{-size.x / 2.f, size.y / 2.f}, a + vec2{size.x / 2.f, size.y / 2.f},
                        a + vec2{size.x / 2.f, -size.y / 2.f}};
                    if (j == 1) {
                        std::swap(q[0], q[3]);
                        std::swap(q[1], q[2]);
                    }
                    for (int32_t k = 0; k < 2; ++k)
                        for (int32_t l = 0; l < 4; ++l) {
                            q[l] += custom::rnv(rnd) * rnd.value();
                            v.heater_quads[j][k][l] = q[l] + custom::rnv(rnd) * rnd.value();
                            q[l] += custom::rnv(rnd) * rnd.value();
                        }
                }
            } else {
                // ElectricGate's constructor
                if (rw::random(s.seed, 0, gid).value() < 0.2f)
                    v.busted_lamp = rw::random(s.seed, 0, rw::ids(gid, 7u)).range(0, 4);
                std::string_view const name = s.to_string_view(w.room_get_name(room.id));
                if (name_is(name, "GATE_SI_CC"))
                    v.meter_height = 558.f;
                else if (name_is(name, "GATE_HI_CC"))
                    v.meter_height = -46.f;
                else if (name_is(name, "GATE_SS_UW") || name_is(name, "GATE_UW_SS"))
                    v.meter_height = -1000.f;
                else
                    v.meter_height = -64.f;
            }
        }

        // WaterGate.Update's setFlows and its water running out (per tick:
        // the counters it reads are scaled to vanilla's)
        void water_gate_flows(gate_visual& v, sys::game_state const& s, dcon::gate_id g) {
            auto const& w = s.world;
            bool const dir = w.gate_get_let_through_dir(g) != 0;
            water_fall& wash = v.falls[dir ? 0 : 1];
            water_fall& outlet = v.falls[dir ? 1 : 0];
            float const pressure = mathf::pow(mathf::inverse_lerp(0.f, 0.5f, v.water_left), 0.6f);
            float const lag_step = 0.016666668f * float(speed_up);
            auto const running = [&](float flow) { v.water_left = mathf::max(0.f, v.water_left - flow * float(speed_up) / 1450.f); };
            vec3 const closed = w.gate_get_door_closed(g);
            vec3 const goal = w.gate_get_door_goal(g);
            switch (w.gate_get_mode(g)) {
            case gate_mode::middle_closed:
                outlet.set_flow = mathf::inverse_lerp(0.f, 60.f, float(w.gate_get_start_counter(g) * speed_up)) * 0.5f * pressure;
                break;
            case gate_mode::closing_air_lock:
                outlet.set_flow = pressure;
                running(1.f);
                break;
            case gate_mode::waiting:
                wash.set_flow = mathf::pow(mathf::inverse_lerp(0.f, 160.f, float(w.gate_get_washing_counter(g) * speed_up)), 1.5f) * 0.5f * pressure;
                v.outlet_lag = mathf::max(v.outlet_lag - lag_step, 0.f);
                outlet.set_flow = v.outlet_lag * pressure;
                break;
            case gate_mode::opening_middle:
                outlet.set_flow = pressure;
                running(1.f);
                if (all_doors_in_position(closed, goal))
                    v.outlet_lag = 1.f;
                break;
            case gate_mode::middle_open:
                v.outlet_lag = mathf::max(v.outlet_lag - lag_step, 0.f);
                outlet.set_flow = v.outlet_lag * pressure;
                break;
            case gate_mode::closing_middle:
                wash.set_flow = pressure;
                outlet.set_flow = 0.f;
                running(1.f);
                break;
            case gate_mode::opening_side:
                wash.set_flow = pressure;
                outlet.set_flow = 0.f;
                running(1.f);
                if (all_doors_in_position(closed, goal)) {
                    wash.set_flow = 0.f;
                    v.outlet_lag = 1.f;
                }
                break;
            case gate_mode::closed:
                v.outlet_lag = mathf::max(v.outlet_lag - lag_step, 0.1f);
                wash.set_flow = v.outlet_lag > 0.05f ? v.outlet_lag * pressure : 0.f;
                running(v.outlet_lag * 0.5f);
                break;
            case gate_mode::broken:
                break;
            }
        }

        // RegionGateGraphics.Update's heaters and steam, one step (no
        // sounds, no bubbles)
        void step_heaters(gate_visual& v, gate_mode mode, bool dir, rw::rng& rnd) {
            int32_t hot = -1;
            for (int32_t l = 0; l < 2; ++l) {
                bool const heating = (mode == gate_mode::closing_air_lock || mode == gate_mode::waiting) && ((l == 0) == dir);
                if (heating)
                    v.heat[l][2] = mathf::min(v.heat[l][2] + 0.0016666667f, 1.f);
                else
                    v.heat[l][2] = mathf::max(v.heat[l][2] - 0.0016666667f, 0.f);
                v.heat[l][0] = mathf::lerp(v.heat[l][0], v.heat[l][2], 0.7f);
                water_fall const& f = v.falls[l];
                if (v.heat[l][0] > 0.f && f.top[0] > v.heater_pos[l].y && f.bottom[0] < v.heater_pos[l].y) {
                    float const r1 = rnd.value(), r2 = rnd.value();
                    if (r1 * r1 < f.flow * 2.f && r2 * r2 < v.heat[l][2] * 2.f) {
                        float const left = float(17 + 9 * l) * 20.f, right = float(22 + 9 * l) * 20.f;
                        vec2 const pos = v.heater_pos[l] + vec2{mathf::lerp(-1.f, 1.f, rnd.value()) * 15.f, mathf::lerp(-1.f, 1.f, rnd.value()) * 10.f};
                        vec2 const vel = custom::deg_to_vec(rnd.value() * 360.f);
                        emit_steam(v, pos, vel, left, 0.f, right, 420.f, mathf::pow(v.heat[l][2], 0.75f), rnd);
                        v.heat[l][0] = mathf::min(v.heat[l][0], (1.f - rnd.value() * f.flow) * v.heat[l][2]);
                    }
                }
                if (v.heat[l][0] > 0.f)
                    hot = l;
            }
            // the heater's LightSource (affectedByPaletteDarkness 0.5)
            if (hot < 0) {
                v.heater_light_alpha = 0.f;
            } else {
                float const h = v.heat[hot][0];
                v.heater_light_alpha = mathf::pow(mathf::inverse_lerp(0.05f, 0.5f, h) * (0.8f + 0.2f * rnd.value()), 0.75f);
                v.heater_light_color = custom::hsl2rgb(mathf::inverse_lerp(0.4f, 0.7f, h) * 0.045f, 1.f,
                    0.5f + 0.1f * mathf::inverse_lerp(0.8f, 1.f, h * (0.7f + rnd.value() * 0.3f)));
                v.heater_light_pos = v.heater_pos[hot];
                v.heater_light_rad = mathf::lerp(200.f, 300.f, std::sin(mathf::pi * h));
            }
        }

        // the electric gate's steam, one step
        void step_electric_steam(gate_visual& v, gate_mode mode, int32_t washing, bool dir, rw::rng& rnd) {
            if (mode == gate_mode::waiting && washing > 0)
                v.electric_steam = mathf::min(1.f, v.electric_steam + 0.025f);
            else
                v.electric_steam = mathf::max(0.f, v.electric_steam - 0.025f);
            if (mathf::pow(rnd.value(), 1.5f) < v.electric_steam * 2.f) {
                int32_t const n = dir ? 0 : 1;
                float const left = float(17 + 9 * n) * 20.f, right = float(22 + 9 * n) * 20.f;
                vec2 const pos = vec2{10.f + (dir ? 19.f : 28.f) * 20.f, 30.f}
                    + vec2{mathf::lerp(-1.f, 1.f, rnd.value()) * 15.f, mathf::lerp(-1.f, 1.f, rnd.value()) * 10.f};
                vec2 const vel = custom::deg_to_vec(rnd.value() * 360.f);
                emit_steam(v, pos, vel, left, 0.f, right, 420.f, mathf::pow(v.electric_steam, 0.75f), rnd);
            }
        }

        // ElectricGate.Update's lamps and battery (per tick)
        void electric_gate_tick(gate_visual& v, sys::game_state const& s, dcon::gate_id g, rw::rng& rnd) {
            auto const& w = s.world;
            ++v.lamp_blink;
            for (int32_t i = 0; i < 4; ++i)
                for (int32_t j = 0; j < 2; ++j) {
                    if (v.lamps_on[i] && i != v.busted_lamp) {
                        v.lamp_alpha[i][j] = (j == 0 ? 1.f : 0.5f) * mathf::lerp(0.65f, 1.f, rnd.value());
                        v.lamp_rad[i][j] = mathf::lerp(v.lamp_rad[i][j], j == 0 ? 100.f : 20.f, 0.5f) * mathf::lerp(0.95f, 1.f, rnd.value());
                    } else {
                        v.lamp_alpha[i][j] = 0.f;
                        v.lamp_rad[i][j] = 0.f;
                    }
                }
            v.battery_changing = false;
            for (int32_t k = 0; k < 4; ++k)
                if (rnd.value() < 0.016666668f)
                    v.lamps_on[k] = false;
            bool const dir = w.gate_get_let_through_dir(g) != 0;
            auto const running = [&] {
                v.battery_left = mathf::max(0.f, v.battery_left - float(speed_up) / 1300.f);
                v.battery_changing = true;
            };
            switch (w.gate_get_mode(g)) {
            case gate_mode::middle_closed:
                if (w.gate_get_start_counter(g) > 0) {
                    v.lamps_on[dir ? 1 : 0] = v.lamp_blink % 20 < 10;
                    v.lamps_on[dir ? 2 : 3] = v.lamp_blink % 20 >= 10;
                    v.lamps_on[!dir ? 1 : 0] = false;
                    v.lamps_on[!dir ? 2 : 3] = false;
                }
                break;
            case gate_mode::closing_air_lock:
            case gate_mode::opening_middle:
                running();
                break;
            case gate_mode::middle_open:
                for (int32_t l = 0; l < 4; ++l)
                    v.lamps_on[dir ? l : 3 - l] = v.lamp_blink % 40 >= l * 10 && v.lamp_blink % 40 < (l + 1) * 10;
                break;
            case gate_mode::closing_middle:
            case gate_mode::opening_side:
                running();
                for (int32_t m = 0; m < 4; ++m)
                    v.lamps_on[m] = v.lamp_blink % 20 < 10;
                break;
            default:
                break;
            }
        }

        // one tick of the gate's drawing state
        void tick_visual(gate_visual& v, sys::game_state const& s, dcon::gate_id g, uint32_t tick) {
            auto const& w = s.world;
            rw::rng rnd = rw::random(s.seed, tick, uint32_t(g.index()));
            for (auto& d : v.doors)
                d.prev = pose_of(d);
            v.heat_prev[0] = v.heat[0][0];
            v.heat_prev[1] = v.heat[1][0];
            gate_mode const mode = w.gate_get_mode(g);
            bool const dir = w.gate_get_let_through_dir(g) != 0;
            vec3 const closed = w.gate_get_door_closed(g);
            vec3 const goal = w.gate_get_door_goal(g);
            if (!v.electric) {
                for (auto& f : v.falls)
                    update_fall(f, v.strike_level);
                water_gate_flows(v, s, g);
            } else {
                electric_gate_tick(v, s, g, rnd);
            }
            for (int32_t step = 1; step <= speed_up; ++step) {
                float const t = float(step) / float(speed_up);
                for (int32_t i = 0; i < 3; ++i)
                    update_door(v.doors[i], v.closed_prev[i] + (closed[i] - v.closed_prev[i]) * t, goal[i]);
                if (!v.electric)
                    step_heaters(v, mode, dir, rnd);
                else
                    step_electric_steam(v, mode, w.gate_get_washing_counter(g), dir, rnd);
            }
            for (int32_t i = 0; i < 3; ++i)
                v.closed_prev[i] = closed[i];
            update_steam(v, rnd);
            v.tick = tick;
        }

        // ---- drawing

        struct palette_ctx {
            sprites::room_palette const& pal;
            vec3 fog;
            vec3 black;
            float fog_amount;
        };

        // the ColoredSprite2 shader's palette colour at depth `d`, row `row`
        vec3 colored2_color(palette_ctx const& pc, float d, int32_t row, vec3 tint) {
            int32_t const col = std::clamp(int32_t(std::floor(0.5f + 29.f * d)), 0, 31);
            vec3 c = pc.pal.texel(col, row);
            if (tint.x < 1.f || tint.y < 1.f || tint.z < 1.f)
                c += (tint - c) * 0.25f;
            c += (pc.fog - c) * (d * pc.fog_amount);
            constexpr float lo = 0.003921568627451f;
            return vec3{mathf::clamp(c.x, lo, 1.f), mathf::clamp(c.y, lo, 1.f), mathf::clamp(c.z, lo, 1.f)};
        }

        cmd colored2(palette_ctx const& pc, std::string_view element, vec2 pos, float rotation, vec2 scale, vec2 anchor, float alpha,
            vec3 tint = vec3{1.f, 1.f, 1.f}) {
            cmd o;
            o.colored2 = true;
            o.c.spr.element = element;
            o.c.spr.pos = pos;
            o.c.spr.rotation = rotation;
            o.c.spr.scale = scale;
            o.c.spr.anchor = anchor;
            o.c.spr.color = tint;
            float const d = 1.f - alpha;
            for (int32_t row = 0; row < 3; ++row) {
                o.pal_near[row] = colored2_color(pc, d, row, tint);
                o.pal_far[row] = colored2_color(pc, 1.f, row, tint);
            }
            return o;
        }

        cmd plain(std::string_view element, vec2 pos, float rotation, vec2 scale, vec2 anchor, vec3 color) {
            cmd o;
            o.c.spr.element = element;
            o.c.spr.pos = pos;
            o.c.spr.rotation = rotation;
            o.c.spr.scale = scale;
            o.c.spr.anchor = anchor;
            o.c.spr.color = color;
            return o;
        }

        // DoorGraphic's sprite indices
        int32_t cog_sprite(int32_t vert, int32_t side, int32_t cog) { return vert * 4 + side * 2 + (1 - cog); }
        int32_t behind_pansar_sprite(int32_t side) { return 8 + side; }
        int32_t pole_sprite(int32_t pole) { return 10 + pole; }
        int32_t track_sprite(int32_t side, int32_t vertical) { return 14 + (vertical + side + side); }
        int32_t center_track_sprite(int32_t vertical) { return 18 + vertical; }
        int32_t clamp_sprite(int32_t side, int32_t c) { return 20 + c * 2 + side; }
        int32_t block_sprite(int32_t side, int32_t block) { return 38 + block + side + side; }
        int32_t hand_sprite(int32_t side, int32_t block) { return 42 + block + side + side; }
        int32_t arm_sprite(int32_t side, int32_t block) { return 46 + block + side + side; }
        int32_t bolt_sprite(int32_t bolt) { return 50 + bolt; }
        int32_t pansar_sprite(int32_t side) { return 54 + side; }
        int32_t pansar_segment_sprite(int32_t segment) { return 56 + segment / 2 + (segment % 2 == 0 ? 0 : 5); }
        int32_t big_screw_sprite(int32_t vertical) { return 65 + vertical; }
        constexpr int32_t door_sprites = 67;

        constexpr std::string_view block_elements[4] = {"RegionGate_Block1", "RegionGate_Block2", "RegionGate_Block3", "RegionGate_Block4"};
        constexpr std::string_view clamp_elements[2][2] = {{"RegionGate_ClampA1", "RegionGate_ClampA2"}, {"RegionGate_ClampB1", "RegionGate_ClampB2"}};
        constexpr std::string_view pansar_elements[2] = {"RegionGate_Pansar1", "RegionGate_Pansar2"};

        // DoorGraphic.DrawSprites (with its ApplyPalette) into its 67 slots
        void draw_door(door_graphic const& d, float closed_now, float a, palette_ctx const& pc, vec3 pole_color, vec3 block_color, cmd (&slot)[door_sprites],
            bool (&used)[door_sprites]) {
            door_pose const& p = d.prev;
            door_pose const c = pose_of(d);
            // camPos += 0.25
            vec2 const nudge{-0.25f, -0.25f};
            vec2 const z = d.pos_z;
            auto put = [&](int32_t i, cmd const& o) {
                slot[i] = o;
                slot[i].c.spr.pos += nudge;
                used[i] = true;
            };
            float const gears = mathf::inverse_lerp(0.2f, 0.9f, closed_now);
            for (int32_t i = 0; i < 2; ++i) {
                {
                    float const num = mathf::pow(mathf::max(0.f, mathf::lerp(p.tracks[1], c.tracks[1], a)), 0.6f);
                    vec2 const at{z.x, z.y + (i == 0 ? num * 65.f : -180.f - num * 130.f)};
                    put(center_track_sprite(i), colored2(pc, i == 0 ? "RegionGate_CenterTrackA" : "RegionGate_CenterTrackB", at, 0.f, vec2{1.f, 1.f},
                                                     vec2{0.5f, i == 0 ? 1.f : 0.f}, 1.f - mathf::lerp(1.5f, 2.5f, num) / 30.f));
                }
                vec2 const vector{z.x, z.y + (i != 0 ? 40.f : -220.f)};
                for (int32_t j = 0; j < 2; ++j) {
                    float const sj = float(-1 + 2 * j), si = float(-1 + 2 * i);
                    float const num = mathf::pow(mathf::max(0.f, mathf::lerp(p.tracks[j * 2], c.tracks[j * 2], a)), 1.4f);
                    put(track_sprite(i, j), colored2(pc, i == 0 ? "RegionGate_TrackA" : "RegionGate_TrackB",
                                                vec2{z.x + (j == 0 ? -9.f : 9.f), z.y + (i == 0 ? num * 130.f : -180.f - num * 65.f)}, 0.f,
                                                vec2{j == 0 ? 1.f : -1.f, 1.f}, vec2{1.f, i == 0 ? 1.f : 0.f}, 1.f - mathf::lerp(1.5f, 2.5f, num) / 30.f));
                    float const num2 = mathf::lerp(p.blocks[i][j], c.blocks[i][j], a);
                    vec2 vector2 = z;
                    vec2 b = custom::deg_to_vec(30.f + num2 * 150.f);
                    b.y += 1.f;
                    b.x *= 20.f * sj * mathf::lerp(num2, 1.f, 0.5f);
                    b.y *= 30.f * si;
                    b.y += 60.f * mathf::inverse_lerp(0.2f, 0.f, num2) * si;
                    vector2 += b;
                    if (i == 0)
                        vector2.y -= 90.f;
                    float const num3 = mathf::pow(std::sin(mathf::pi * num2), 3.f) * -2.f * sj * si;
                    put(block_sprite(i, j), colored2(pc, block_elements[3 - (i + i + (1 - j))], vector2, num3, vec2{1.f, 1.f}, vec2{j == 0 ? 1.f : 0.f, 1.f},
                                                1.f - mathf::lerp(3.f, 2.f, mathf::pow(num2, 7.f)) / 30.f, block_color));
                    float const num4 = mathf::lerp(0.f, 100.f * (i == 0 ? -1.f : 1.f), mathf::pow(mathf::max(0.f, mathf::lerp(p.arms[i][j], c.arms[i][j], a)), 0.8f));
                    float const y = mathf::lerp(-10.f, -80.f, i == 0 ? 0.f : 1.f);
                    vec2 vector3 = vector2 + rotate_around_origo(vec2{22.f * sj, y}, num3);
                    vector3.y += num4;
                    vec2 vector4 = z
                        + vec2{sj * mathf::lerp(30.f, -35.f, mathf::pow(0.5f * num2 + 0.5f * std::sin(num2 * mathf::pi), 1.f + 4.f * num2)), i == 1 ? 60.f : -240.f};
                    vector4.y += num4;
                    float const num5 = circles_collision_time(vector3.x, vector3.y, vector.x, vector.y, vector4.x - vector3.x, vector4.y - vector3.y, 1.f, 28.f);
                    if (num5 > 0.f && num5 < 1.f)
                        vector4 = rw::lerp(vector3, vector4, num5);
                    put(arm_sprite(i, j), colored2(pc, "RegionGate_Pixel", vector3, custom::aim(vector3, vector4), vec2{3.f, rw::distance(vector3, vector4)},
                                              vec2{0.5f, 0.f}, 0.93333334f));
                    put(hand_sprite(i, j), colored2(pc, "RegionGate_Hand", vector3, num3, vec2{j == 0 ? 1.f : -1.f, 1.f}, vec2{0.5f, 0.5f}, 0.93333334f));
                }
                for (int32_t k = 0; k < 9; ++k) {
                    vec2 const at = rw::lerp(p.clamp_pos[i][k], c.clamp_pos[i][k], a);
                    put(clamp_sprite(i, k), colored2(pc, clamp_elements[k % 2][i], at, mathf::lerp(p.clamp_rot[i][k], c.clamp_rot[i][k], a), vec2{1.f, 1.f},
                                                vec2{i == 0 ? 1.f : 0.f, 0.f}, 1.f - d.clamps[i][k].depth));
                }
                float const num6 = mathf::lerp(p.pc, c.pc, a);
                vec2 vector5 = z;
                vector5.x += float(-1 + i * 2) * 7.f * num6;
                vector5.x += std::sin(num6 * mathf::pi) * 25.f * (i == 0 ? -1.f : 1.f);
                vector5.y -= 90.f;
                float const pansar_alpha = mathf::lerp(0.7f, 1.f, num6);
                if (num6 > 0.5f)
                    put(pansar_sprite(i), colored2(pc, pansar_elements[i], vector5, 0.f, vec2{1.f, 1.f}, vec2{i == 0 ? 1.f : 0.f, 0.5f}, pansar_alpha));
                put(behind_pansar_sprite(i), colored2(pc, pansar_elements[i], vector5, 0.f, vec2{1.f, 1.f}, vec2{i == 0 ? 1.f : 0.f, 0.5f}, pansar_alpha));
                put(big_screw_sprite(i), colored2(pc, "RegionGate_BigScrew", vector, mathf::lerp(p.screws[i], c.screws[i], a), vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, 1.f));
                for (int32_t l = 0; l < 2; ++l)
                    for (int32_t m = 0; m < 2; ++m) {
                        vec2 vector6 = z;
                        vector6.y -= 90.f + (i == 0 ? -1.f : 1.f) * (m == 0 ? 150.f : 175.f) * (i == 0 ? 1.f : 1.2f);
                        vector6.x += (l == 0 ? -1.f : 1.f) * (m == 0 ? 40.f : 50.f) * (i == 0 ? 1.f : 0.8f);
                        float const rot = (l == 0 ? -1.f : 1.f) * (i == 0 ? 1.f : -1.f) * (gears * 0.5f + 0.5f * std::sin(gears * mathf::pi)) * (m == 0 ? 90.f : 210.f);
                        put(cog_sprite(i, l, m), colored2(pc, "RegionGate_Cog", vector6, rot, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, 1.f - (m == 0 ? 12.f : 15.f) / 30.f));
                    }
            }
            for (int32_t n = 0; n < 9; ++n) {
                float const pl = mathf::lerp(p.pansar[n], c.pansar[n], a);
                float const rot = n % 2 == 1 ? 90.f * mathf::lerp(p.pansar_rot[n], c.pansar_rot[n], a) * ((((n / 2) % 2 == 0) == d.flip) ? -1.f : 1.f) : 0.f;
                put(pansar_segment_sprite(n), colored2(pc, n % 2 == 0 ? "RegionGate_PansarSegment" : "RegionGate_PansarLock", vec2{z.x, z.y - 180.f * pl}, rot,
                                                  vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, mathf::lerp(1.f, 0.8f, mathf::inverse_lerp(0.3f, -0.2f, pl))));
            }
            for (int32_t k = 0; k < 4; ++k) {
                if (closed_now == 1.f || (d.poles[k][1] == 1.f && d.poles[k][0] == 1.f))
                    continue;
                float const x = z.x + (((k > 0 && k < 3) ? 14.f : 11.f) + (k < 2 ? 1.f : 0.f)) * (float(k) - 1.5f);
                float const sgn = ((k % 2 == 0) == d.flip) ? -1.f : 1.f;
                float const y = z.y - 90.f + sgn * 200.f * mathf::pow(mathf::max(0.f, mathf::lerp(p.poles[k], c.poles[k], a)), 1.4f);
                put(pole_sprite(k), plain("pixel", vec2{x, y}, 0.f, vec2{3.f, 200.f}, vec2{0.5f, 0.5f}, pole_color));
            }
            for (int32_t k = 0; k < 4; ++k)
                if (d.bolts[k])
                    put(bolt_sprite(k), colored2(pc, "RegionGate_Bolt", vec2{z.x, z.y - 30.f - 40.f * float(k)}, 0.f, vec2{1.f, 1.f}, vec2{0.5f, 0.5f}, 0.93333334f));
        }

        float hash01(uint32_t x, uint32_t y) {
            return rw::keyed::to_value(rw::keyed::mix(x * rw::keyed::k_tick ^ y * rw::keyed::k_id ^ 0x5bd1e995u));
        }
        // 1D value noise along `t` for the column `col` (stands in for the
        // WaterFall shader's _NoiseTex, bilinearly sampled)
        float column_noise(uint32_t col, float t) {
            float const f = std::floor(t);
            float const u = t - f;
            int32_t const i = int32_t(f);
            float const a = hash01(col, uint32_t(i)), b = hash01(col, uint32_t(i + 1));
            return a + (b - a) * u;
        }

        // WaterFall.DrawSprites with the WaterFall shader's look: the
        // stream's rectangle in 4 x 6 px cells, each shown where the shader
        // keeps its pixels (the edges' fade, the visual density) in the water
        // colours, the noise scrolling down
        void draw_fall(water_fall const& f, float strike, float a, float rain, vec3 deep, vec3 shallow, std::vector<cmd>& out) {
            if (strike > f.pos.y)
                return; // Flooded
            float const top = mathf::lerp(f.top[1], f.top[0], a);
            float const bottom = mathf::lerp(f.bottom[1], f.bottom[0], a);
            float const dens = mathf::lerp(f.last_visual_dens, f.visual_density, a);
            if (top - bottom < 1.f || dens <= 0.f)
                return;
            float const float_length = std::abs(f.pos.y - strike);
            float const n3 = mathf::min((top - bottom) / float_length, 1.f);
            float n4 = mathf::inverse_lerp(f.pos.y, strike, top);
            float n5 = mathf::inverse_lerp(strike, f.pos.y, bottom);
            n4 = mathf::lerp(mathf::pow(n4, 0.2f), 1.f, 1.f - n3);
            n5 = mathf::lerp(mathf::pow(n5, 0.2f), 1.f, 1.f - n3);
            float const k_top = mathf::lerp(100.f, 2.f, n4);
            float const k_bottom = mathf::lerp(100.f, 2.f, n5);
            constexpr int32_t cols = 15;
            constexpr float cw = 60.f / float(cols);
            constexpr float ch = 6.f;
            int32_t const rows = int32_t(std::ceil((top - bottom) / ch));
            for (int32_t col = 0; col < cols; ++col) {
                float const u = (float(col) + 0.5f) / float(cols);
                float const x0 = f.pos.x + float(col) * cw;
                float const ex = u < 0.5f ? u * 10.f : (1.f - u) * 10.f;
                uint32_t const ncol = uint32_t(int32_t(std::floor(x0 / cw)));
                // runs of cells of one colour, one quad each
                float run_top = 0.f, run_bottom = 0.f;
                float run_grad = -1.f;
                auto flush = [&] {
                    if (run_grad < 0.f)
                        return;
                    vec3 const colr = deep + (shallow - deep) * run_grad;
                    cmd o;
                    sprites::draw_cmd& dc = o.c;
                    dc.is_quad = true;
                    dc.untextured = true;
                    dc.vertex_colors = true;
                    dc.quad[0] = vec2{x0, run_top};
                    dc.quad[1] = vec2{x0 + cw, run_top};
                    dc.quad[2] = vec2{x0 + cw, run_bottom};
                    dc.quad[3] = vec2{x0, run_bottom};
                    for (auto& vc : dc.vcol)
                        vc = rw::vec4{colr.x, colr.y, colr.z, 1.f};
                    out.push_back(o);
                    run_grad = -1.f;
                };
                for (int32_t row = 0; row < rows; ++row) {
                    float const y0 = top - float(row) * ch;
                    float const y1 = mathf::max(bottom, y0 - ch);
                    float const ym = (y0 + y1) * 0.5f;
                    float const v = (top - ym) / (top - bottom);
                    float const edge = mathf::min(ex, v < 0.5f ? v * k_top : (1.f - v) * k_bottom);
                    // _NoiseTex at (x * 15, 1.8 _RAIN + y * 0.2) in screen units, 256 texels a unit
                    float const noise = column_noise(ncol, (1.8f * rain + ym / 768.f * 0.2f) * 256.f);
                    float const sincol = std::sin((0.8f * rain + noise * 3.f) * 3.14f * 2.f) * 0.5f + 0.5f;
                    bool const shown = (edge + (sincol - edge) * 0.5f) >= 0.5f && sincol >= 1.f - dens;
                    if (!shown) {
                        flush();
                        continue;
                    }
                    float const grad = mathf::pow(std::floor((0.5f + (sincol - 0.5f) * 0.2f) * 10.f) / 10.f, 0.7f);
                    if (run_grad >= 0.f && grad == run_grad) {
                        run_bottom = y1;
                        continue;
                    }
                    flush();
                    run_grad = grad;
                    run_top = y0;
                    run_bottom = y1;
                }
                flush();
            }
        }

        // the part of a Steam shader sprite's radius that shows at `life`
        // (its alpha): the shader keeps h >= 0.35, h about lerp(h0 dist,
        // lerp(h0, 1, 0.8), dist) life with dist = (1 - 2 r) sqrt(life) at r
        // of the radius, and h0 its noise (0..1; 0.75 here, where its
        // bright bands reach: 1.7 dist - 0.75 dist^2 >= 0.35 / life)
        float steam_visible(float life) {
            if (life <= 0.f)
                return 0.f;
            float const k = 0.35f / life;
            float const disc = 2.89f - 3.f * k;
            if (disc < 0.f)
                return 0.f;
            float const dist = (1.7f - std::sqrt(disc)) / 1.5f;
            float const top = std::sqrt(life);
            if (dist >= top)
                return 0.f;
            return 1.f - dist / top;
        }

        cmd circle(vec2 pos, float rad, vec3 color, float jag, uint32_t seed) {
            cmd o;
            o.c.is_circle = true;
            o.c.spr.pos = pos;
            o.c.spr.scale = vec2{rad, rad};
            o.c.color = color;
            o.c.circle_jag = jag;
            o.c.circle_seed = seed;
            return o;
        }

        // SpriteSmoke.DrawSprites / SteamParticle with the Steam shader's
        // look: discs (the stretched sprite towards the particle it trails
        // to as discs along it), jagged where the shader's noise eats in
        void draw_steam(gate_visual& v, float a, vec3 color, std::vector<cmd>& out) {
            for (auto const& p : v.steam) {
                if (!p.alive)
                    continue;
                float const life = mathf::lerp(p.last_life, p.life, a);
                float const str = mathf::lerp(p.last_stretched, p.stretched, a);
                float const vis = steam_visible(life);
                if (vis <= 0.f)
                    continue;
                vec2 const pos = rw::lerp(p.last_pos, p.pos, a);
                vec2 linger = rw::lerp(p.last_linger, p.linger, a);
                if (steam_particle const* nx = next_of(v, p))
                    linger = rw::lerp(nx->last_pos, nx->pos, a);
                float const num = mathf::pow(mathf::max(0.f, mathf::lerp(std::sin(life * mathf::pi), 1.f - life, 0.7f)), 0.8f);
                float const rad0 = mathf::lerp(4.f, p.rad, num + str);
                float const rad1 = 1.5f * mathf::lerp(2.f, p.rad, num);
                float const rad2 = mathf::lerp(4.f, p.rad, num);
                // sprite 0: an ellipse rad0 wide, the distance + rad2 long,
                // centred between the particle and the one it trails to
                float const len = rw::distance(pos, linger) + rad2;
                vec2 const mid = rw::lerp(pos, linger, 0.5f);
                vec2 const dir = custom::dir_vec(pos, linger);
                int32_t const n = std::clamp(int32_t(len / mathf::max(rad0 * 0.5f, 4.f)), 2, 8);
                for (int32_t k = 0; k < n; ++k) {
                    float const t = (float(k) + 0.5f) / float(n) * 2.f - 1.f;
                    float const r = rad0 * 0.5f * vis * std::sqrt(mathf::max(0.f, 1.f - t * t));
                    if (r < 1.f)
                        continue;
                    out.push_back(circle(mid + dir * (t * len * 0.5f * vis), r, color, 0.25f, p.id * 16u + uint32_t(k)));
                }
                // sprite 1: the disc at the particle
                float const r1 = rad1 * 0.5f * vis;
                if (r1 >= 1.f)
                    out.push_back(circle(pos, r1, color, 0.25f, p.id * 16u + 15u));
            }
        }

        // RegionGate_Heater (TriangleMesh.MakeGridMesh over QuadGridMesh's
        // corners: one quad here, the corners hardly off a rectangle)
        void draw_heater(gate_visual const& v, int32_t j, float a, palette_ctx const& pc, std::vector<cmd>& out) {
            float const h = mathf::lerp(v.heat_prev[j], v.heat[j][0], a);
            // HeaterColor
            vec3 color = pc.black + (vec3{1.f, 0.f, 0.f} - pc.black) * mathf::inverse_lerp(0.f, 0.3f, h);
            if (h > 0.3f)
                color = custom::hsl2rgb(mathf::inverse_lerp(0.3f, 1.f, h) * 0.16f, 1.f, 0.5f + 0.2f * mathf::inverse_lerp(0.3f, 1.f, h));
            vec3 cols[2];
            cols[0] = color;
            vec3 const dim = pc.black + (pc.fog - pc.black) * mathf::lerp(0.3f, 0.8f, h);
            cols[1] = color + (dim - color) * mathf::lerp(0.8f, 0.1f, h);
            // HeaterSprite(j, part) = 1 - part after the doors: part 1 first
            for (int32_t k = 1; k >= 0; --k) {
                vec2 const* q = v.heater_quads[j][k];
                cmd o;
                sprites::draw_cmd& dc = o.c;
                dc.spr.element = "RegionGate_Heater";
                dc.is_quad = true;
                dc.vertex_colors = true;
                // QuadGridMesh: quad 0 the bottom left of the element, 1 top
                // left, 2 top right, 3 bottom right
                dc.quad[0] = q[1];
                dc.quad[1] = q[2];
                dc.quad[2] = q[3];
                dc.quad[3] = q[0];
                for (auto& vc : dc.vcol)
                    vc = rw::vec4{cols[k].x, cols[k].y, cols[k].z, 1.f};
                out.push_back(o);
            }
        }
    }

    void build(sys::game_state const& s, world::room_view const& room, float alpha, sprites::room_palette const& pal, rw::vec2 heater_size,
        std::vector<cmd>& out) {
        auto const& w = s.world;
        auto const g = gates::gate_of(s, room.id);
        if (!g)
            return;
        auto& map = visuals();
        uint32_t const gid = uint32_t(g.index());
        auto it = map.find(gid);
        bool fresh = it == map.end();
        if (fresh)
            it = map.emplace(gid, gate_visual{}).first;
        gate_visual& v = it->second;
        if (!fresh && (v.room != room.id || v.electric != (w.gate_get_electric(g) != 0) || s.tick < v.tick || s.tick - v.tick > max_catch_up))
            fresh = true;
        if (fresh) {
            init_visual(v, s, room, g, heater_size);
            // not vanilla (its graphics run from the room's realisation): a
            // gate first seen mid-cycle warms up in its current mode, so the
            // streams, steam and heaters are already going (the last tick is
            // this one, something to draw from)
            constexpr uint32_t warm_up = 160;
            uint32_t const from = s.tick > warm_up ? s.tick - warm_up : 0;
            for (uint32_t t = from; t <= s.tick; ++t)
                tick_visual(v, s, g, t);
            for (auto& d : v.doors)
                d.prev = pose_of(d);
            v.heat_prev[0] = v.heat[0][0];
            v.heat_prev[1] = v.heat[1][0];
        } else {
            while (v.tick < s.tick)
                tick_visual(v, s, g, v.tick + 1);
        }

        // ApplyPalette
        palette_ctx const pc{pal, pal.fog(), pal.black(), [&] {
                                 vec3 const p = pal.texel(9, 7);
                                 if (p.x == 0.f && p.y == 0.f && p.z > 0.f)
                                     return 1.f + p.z;
                                 return 1.f - p.x;
                             }()};
        vec3 const pole_color = pc.black + (pc.fog - pc.black) * 0.12f;
        vec3 const block_target = pal.texel(6, 4) + (pc.fog - pal.texel(6, 4)) * 0.5f;
        vec3 const block_color = vec3{0.05f, 0.05f, 0.f} + (block_target - vec3{0.05f, 0.05f, 0.f}) * 0.25f;
        vec3 const closed = w.gate_get_door_closed(g);

        // the Background container: the water streams
        if (!v.electric) {
            float const rain = float(std::fmod((double(s.tick) + double(alpha)) / 40.0 / 5.0, 2000.0));
            for (auto const& f : v.falls)
                draw_fall(f, v.strike_level, alpha, rain, pal.texel(5, 7), pal.texel(4, 7), out);
        }

        // the doors' sprites, then the Midground's (the poles, the first
        // heater) before the rest (the Water container)
        static cmd slots[3][door_sprites];
        static bool used[3][door_sprites];
        for (int32_t i = 0; i < 3; ++i) {
            std::fill(std::begin(used[i]), std::end(used[i]), false);
            draw_door(v.doors[i], closed[i], alpha, pc, pole_color, block_color, slots[i], used[i]);
        }
        auto const midground = [](int32_t k) { return k >= 10 && k <= 13; };
        for (int32_t i = 0; i < 3; ++i)
            for (int32_t k = 0; k < door_sprites; ++k)
                if (used[i][k] && midground(k))
                    out.push_back(slots[i][k]);
        if (!v.electric)
            draw_heater(v, 0, alpha, pc, out);

        // the Foreground container: the battery meter (CustomDepth: its
        // colour, opaque)
        if (v.electric) {
            rw::rng rnd = rw::random(s.seed, s.tick, gid);
            float const changing = v.battery_changing ? 1.f : 0.f;
            float const width = 420.f * v.battery_left - (v.battery_changing ? 5.f * rnd.value() : 0.f);
            vec3 const hot = custom::hsl2rgb(0.03f + rnd.value() * (0.035f * changing + 0.025f), 1.f,
                (0.5f + rnd.value() * 0.2f * changing) * mathf::lerp(1.f, 0.25f, pal.darkness()));
            vec3 const color = hot + (pc.black - hot) * 0.5f;
            if (width > 0.f)
                out.push_back(plain("pixel", vec2{480.f, v.meter_height}, 0.f, vec2{width, 8.f}, vec2{0.5f, 0.5f}, color));
        }

        // the Water container
        for (int32_t i = 0; i < 3; ++i)
            for (int32_t k = 0; k < door_sprites; ++k)
                if (used[i][k] && !midground(k))
                    out.push_back(slots[i][k]);
        if (!v.electric)
            draw_heater(v, 1, alpha, pc, out);
        // (the heat distortion over the hot heater, a GrabShaders sprite, isn't drawn)

        // the steam (SteamParticle: the Water container)
        vec3 const steam = pc.fog + (vec3{1.f, 1.f, 1.f} - pc.fog) * mathf::lerp(0.03f, 0.35f, pal.texel(30, 7).x);
        // (the Steam shader shades it by the level's depth behind: half way)
        draw_steam(v, alpha, steam * 0.8f, out);
    }

    void lights(sys::game_state const& s, dcon::room_id room, std::vector<light>& out) {
        auto const g = gates::gate_of(s, room);
        if (!g)
            return;
        auto const& map = visuals();
        auto const it = map.find(uint32_t(g.index()));
        if (it == map.end() || it->second.room != room)
            return;
        gate_visual const& v = it->second;
        if (v.electric) {
            // ElectricGate.lamps (affectedByPaletteDarkness 0)
            for (int32_t i = 0; i < 4; ++i) {
                bool const low = i < 2;
                bool right = i % 2 == 0;
                if (low)
                    right = !right;
                vec2 const pos{10.f + (right ? 19.f : 28.f) * 20.f, 10.f + (low ? 9.f : 15.f) * 20.f};
                for (int32_t j = 0; j < 2; ++j)
                    if (v.lamp_alpha[i][j] > 0.f && v.lamp_rad[i][j] > 0.f)
                        out.push_back(light{pos, v.lamp_rad[i][j], v.lamp_alpha[i][j], vec3{1.f, j == 0 ? 0.4f : 0.6f, 0.f}, j == 1, 0.f});
            }
        } else if (v.heater_light_alpha > 0.f) {
            out.push_back(light{v.heater_light_pos, v.heater_light_rad, v.heater_light_alpha, v.heater_light_color, false, 0.5f});
        }
    }
}
