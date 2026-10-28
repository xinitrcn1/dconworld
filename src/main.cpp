#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "config.h"
#include "gamestate.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "render.hpp"
#include "reports.hpp"
#include "selftest.hpp"
#include "world.hpp"

namespace {
    using rw::vec2;

    std::string find_streaming_assets() {
        if (char const* env = std::getenv("RW_ROOT"); env != nullptr && *env != '\0')
            return env;
        std::string const home = std::getenv("HOME") != nullptr ? std::getenv("HOME") : "";
        for (auto const& base : {home + "/.local/share/Steam/steamapps/common/Rain World",
                 home + "/.steam/steam/steamapps/common/Rain World",
                 home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common/Rain World"}) {
            auto const sa = base + "/RainWorld_Data/StreamingAssets";
            if (std::filesystem::exists(sa + "/world/regions.txt"))
                return sa;
        }
        return {};
    }

    void print_usage(char const* argv0) {
        std::printf(
            "usage: %s [options]\n"
            "  --root <dir>        add a StreamingAssets-like root (repeatable, first has priority)\n"
            "                      default: $RW_ROOT or the Steam install\n"
            "  --room <name>       start in this room (default su_a22)\n"
            "  --slugcat <name>    white | yellow | red\n"
            "  --selftest          run the scripted movement tests and exit\n"
            "  --fuzz [ticks]      soak test the slugcat in every room and exit\n"
            "  --screenshot <bmp>  render one frame after --ticks idle ticks, save it and exit\n"
            "  --ticks <n>         ticks to simulate before --screenshot (default 80)\n"
            "  --script <s>        inputs before --screenshot, e.g. U2,N30,RJ12: letters\n"
            "                      U D L R J (N = nothing) held for the given ticks\n"
            "\n"
            "controls: arrows/WASD move, Z/K/Space jump, R respawn, left click teleport,\n"
            "          PageUp/PageDown room, Home/End region, C camera mode, F1 HUD, F2 camera outlines,\n"
            "          G collision geometry overlay, B day/night,\n"
            "          F5 add a co-op player (gamepad), P pause, N single step, T slow motion,\n"
            "          1/2/3 slugcat, Esc quit\n",
            argv0);
    }

    struct app {
        sys::game_state& gs;
        std::vector<dcon::player_id> players; // co-op slots, [0] is the keyboard
        player::slugcat_name slugcat = player::slugcat_name::white;
        dcon::room_id room;
        vec2 spawn{0.f, 0.f};
        render::camera cam;
        uint32_t camera_index = 0;
        bool follow_camera = false;

        explicit app(sys::game_state& gs) : gs{gs} {}

        world::room_view view() const { return world::get_room_view(gs, room); }

        std::vector<vec2> camera_positions() const {
            std::vector<vec2> r;
            gs.world.room_for_each_camera_location(room, [&](dcon::camera_location_id cl) {
                r.push_back(gs.world.room_camera_get_position(gs.world.camera_location_get_camera(cl)));
            });
            return r;
        }

        void enter_room(dcon::room_id rid) {
            room = rid;
            camera_index = 0;
            auto const cams = camera_positions();
            vec2 const first = cams.empty() ? vec2{0.f, 0.f} : cams[0];
            spawn = selftest::find_spawn(view(), first + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f});
            for (size_t i = 0; i < players.size(); ++i) {
                player::reset(gs, players[i], rid, spawn + vec2{float(i) * 20.f, 0.f});
                player_graphics::reset(gs, players[i]);
            }
            auto const& w = gs.world;
            reports::add_log("room " + std::string{gs.to_string_view(w.room_get_display_name(rid))} + " ("
                + std::string{gs.to_string_view(w.region_get_name(w.room_get_region_from_region_membership(rid)))} + ") "
                + std::to_string(w.room_get_width(rid)) + "x" + std::to_string(w.room_get_height(rid)));
        }

        // Snap to the vanilla camera containing player one, like RoomCamera
        void update_camera(float view_w, float view_h) {
            vec2 const head = gs.world.body_chunk_get_pos(player::body_chunk(gs, players[0], 0));
            if (follow_camera) {
                cam.pos = head - vec2{view_w / 2.f, view_h / 2.f};
                return;
            }
            auto const cams = camera_positions();
            if (cams.empty())
                return;
            auto const inside = [&](uint32_t i, float margin) {
                vec2 const c = cams[i];
                return head.x > c.x + margin && head.x < c.x + gs.screen_width - margin
                    && head.y > c.y + margin && head.y < c.y + gs.screen_height - margin;
            };
            if (camera_index >= cams.size() || !inside(camera_index, 0.f)) {
                float best = std::numeric_limits<float>::max();
                for (uint32_t i = 0; i < cams.size(); ++i) {
                    vec2 const center = cams[i] + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f};
                    float const d = inside(i, 20.f) ? -1.f : rw::distance(center, head);
                    if (d < best) {
                        best = d;
                        camera_index = i;
                    }
                }
            }
            cam.pos = cams[camera_index];
        }

        std::string hud(bool paused, bool slow) const {
            auto const& w = gs.world;
            auto const p = players[0];
            auto const c0 = player::body_chunk(gs, p, 0);
            auto const c1 = player::body_chunk(gs, p, 1);
            char buf[768];
            std::snprintf(buf, sizeof(buf),
                "%s  [%s]%s%s\n"
                "mode %s  anim %s\n"
                "vel %.2f %.2f  feet %.0f %.0f\n"
                "contact %d %d / %d %d  slope %d\n"
                "standing %d canJump %d wantJump %d stun %d\n"
                "air %.2f  aerobic %.2f  players %zu%s",
                std::string{gs.to_string_view(w.room_get_display_name(room))}.c_str(),
                std::string{gs.to_string_view(w.region_get_display_name(w.room_get_region_from_region_membership(room)))}.c_str(),
                paused ? "  PAUSED" : "", slow ? "  SLOW" : "",
                player::to_string(w.player_get_body_mode(p)), player::to_string(w.player_get_animation(p)),
                w.body_chunk_get_vel(c0).x, w.body_chunk_get_vel(c0).y, w.body_chunk_get_pos(c1).x, w.body_chunk_get_pos(c1).y,
                w.body_chunk_get_contact_point(c0).x, w.body_chunk_get_contact_point(c0).y,
                w.body_chunk_get_contact_point(c1).x, w.body_chunk_get_contact_point(c1).y, int(w.body_chunk_get_on_slope(c1)),
                int(w.player_get_standing(p)), w.player_get_can_jump(p), w.player_get_want_to_jump(p), w.player_get_stun(p),
                w.player_get_air_in_lungs(p), w.player_get_aerobic_level(p), players.size(), w.player_get_dead(p) ? "  DEAD (R)" : "");
            return buf;
        }
    };

    player::input_package keyboard_input() {
        bool const* keys = SDL_GetKeyboardState(nullptr);
        float ax = 0.f, ay = 0.f;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) ax -= 1.f;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) ax += 1.f;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) ay -= 1.f;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) ay += 1.f;
        bool const jmp = keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_K] || keys[SDL_SCANCODE_SPACE];
        return player::make_input(ax, ay, jmp, keys[SDL_SCANCODE_X], keys[SDL_SCANCODE_LSHIFT]);
    }

    player::input_package gamepad_input(SDL_Gamepad* pad) {
        if (pad == nullptr)
            return player::input_package{};
        float ax = float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.f;
        float ay = -float(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY)) / 32767.f;
        if (std::fabs(ax) < 0.15f && std::fabs(ay) < 0.15f)
            ax = ay = 0.f;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) ax = -1.f;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) ax = 1.f;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) ay = -1.f;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP)) ay = 1.f;
        return player::make_input(ax, ay, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST), SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
    }
}

int main(int argc, char* argv[]) {
    std::vector<std::string> roots;
    std::string start_room = "su_a22";
    std::string screenshot;
    uint32_t screenshot_ticks = 80;
    std::string script;
    bool selftest_mode = false;
    int32_t fuzz_ticks = -1;
    player::slugcat_name slugcat = player::slugcat_name::white;
    for (int i = 1; i < argc; ++i) {
        std::string const a = argv[i];
        auto const next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--root") {
            roots.push_back(next());
        } else if (a == "--room") {
            start_room = next();
        } else if (a == "--slugcat") {
            auto const n = next();
            slugcat = n == "yellow" ? player::slugcat_name::yellow : n == "red" ? player::slugcat_name::red : player::slugcat_name::white;
        } else if (a == "--selftest") {
            selftest_mode = true;
        } else if (a == "--fuzz") {
            fuzz_ticks = 400;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                fuzz_ticks = std::atoi(argv[++i]);
        } else if (a == "--screenshot") {
            screenshot = next();
        } else if (a == "--script") {
            script = next();
        } else if (a == "--ticks") {
            screenshot_ticks = uint32_t(std::atoi(next().c_str()));
        } else {
            print_usage(argv[0]);
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }

    if (selftest_mode)
        return selftest::run();

    auto const gs_ptr = sys::make_game_state();
    auto& gs = *gs_ptr;
    if (roots.empty()) {
        auto const sa = find_streaming_assets();
        if (sa.empty()) {
            reports::add_log("can't find Rain World's StreamingAssets, pass --root or set RW_ROOT");
            return 1;
        }
        roots.push_back(sa);
    }
    for (auto const& r : roots) {
        reports::add_log("root: " + r);
        gs.fs.add_root(r);
    }
    // our own assets (HUD font) come last so the game data always wins
    if (char const* base = SDL_GetBasePath(); base != nullptr)
        gs.fs.add_root(std::string{base} + "assets");
    gs.fs.generate_unpopulated_tree();
    parsers::error_handler err;
    world::load(gs, err);
    if (!err.accumulated_errors.empty())
        reports::add_log("errors:\n" + err.accumulated_errors);
    if (!err.accumulated_warnings.empty())
        reports::add_log("warnings:\n" + err.accumulated_warnings);
    if (err.fatal || gs.world.room_size() == 0)
        reports::error_fatal("no rooms loaded");

    if (fuzz_ticks >= 0)
        return selftest::fuzz(gs, uint32_t(fuzz_ticks));

    app a{gs};
    a.slugcat = slugcat;
    dcon::room_id start = gs.find_room(start_room);
    if (!start) {
        reports::add_log("unknown room \"" + start_room + "\", using the first one");
        start = dcon::room_id{0};
    }
    a.players.push_back(player::create(gs, start, vec2{0.f, 0.f}, slugcat));
    a.enter_room(start);

    render::state r;
    bool const headless = !screenshot.empty();
    if (!render::init(r, int(gs.screen_width), int(gs.screen_height), "rwcpp", headless))
        return 1;
    render::load_assets(r, gs);

    if (headless) {
        auto const step = [&](player::input_package const& in) {
            player::update(gs, a.players[0], a.view(), in);
            player_graphics::update(gs, a.players[0], a.view());
        };
        for (uint32_t t = 0; t < screenshot_ticks; ++t)
            step(player::input_package{});
        // scripted input: comma separated "<letters><ticks>"
        size_t pos = 0;
        while (pos < script.size()) {
            size_t const end = std::min(script.find(',', pos), script.size());
            std::string const tok = script.substr(pos, end - pos);
            pos = end + 1;
            float ax = 0.f, ay = 0.f;
            bool jmp = false;
            size_t i = 0;
            for (; i < tok.size() && std::isalpha(static_cast<unsigned char>(tok[i])); ++i) {
                switch (tok[i]) {
                case 'U': ay += 1.f; break;
                case 'D': ay -= 1.f; break;
                case 'L': ax -= 1.f; break;
                case 'R': ax += 1.f; break;
                case 'J': jmp = true; break;
                default: break;
                }
            }
            int const n = i < tok.size() ? std::atoi(tok.c_str() + i) : 1;
            for (int k = 0; k < n; ++k)
                step(player::make_input(ax, ay, jmp, false, false));
        }
        render::use_room(r, gs, a.room);
        a.update_camera(float(r.width), float(r.height));
        render::draw_frame(r, gs, a.view(), 1.f, a.cam, a.hud(false, false));
        bool const ok = render::save_screenshot(r, screenshot.c_str());
        render::shutdown(r);
        reports::add_log(ok ? "saved " + screenshot : std::string{"screenshot failed"});
        return ok ? 0 : 1;
    }

    SDL_Gamepad* pad = nullptr;
    bool running = true;
    bool paused = false;
    bool slow = false;
    bool step_once = false;
    uint64_t const tick_ns = 1000000000ull / gs.ticks_per_second;
    uint64_t last = SDL_GetTicksNS();
    uint64_t accumulator = 0;
    auto const tick = [&] {
        auto const view = a.view();
        for (size_t i = 0; i < a.players.size(); ++i) {
            // single player: keyboard or gamepad; co-op: gamepad drives player two
            player::input_package in = keyboard_input();
            if (a.players.size() == 1) {
                auto const g = gamepad_input(pad);
                if (g.analogue_dir != vec2{0.f, 0.f} || g.jmp)
                    in = g;
            } else if (i > 0) {
                in = i == 1 ? gamepad_input(pad) : player::input_package{};
            }
            player::update(gs, a.players[i], view, in);
            player_graphics::update(gs, a.players[i], view);
        }
    };
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (pad == nullptr)
                    pad = SDL_OpenGamepad(e.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad != nullptr && SDL_GetGamepadID(pad) == e.gdevice.which) {
                    SDL_CloseGamepad(pad);
                    pad = nullptr;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    player::reset(gs, a.players[0], a.room, render::screen_to_room(r, a.cam, e.button.x, e.button.y));
                    player_graphics::reset(gs, a.players[0]);
                }
                break;
            case SDL_EVENT_KEY_DOWN: {
                if (e.key.repeat)
                    break;
                auto& w = gs.world;
                uint32_t const n = w.room_size();
                auto const room_at = [](uint32_t i) { return dcon::room_id{dcon::room_id::value_base_t(i)}; };
                auto const region_first = [&](int32_t delta) {
                    uint32_t const rc = w.region_size();
                    uint32_t reg = uint32_t(w.room_get_region_from_region_membership(a.room).index());
                    for (uint32_t k = 0; k < rc; ++k) {
                        reg = (reg + rc + uint32_t(delta)) % rc;
                        auto const range = w.region_range_of_region_membership(dcon::region_id{dcon::region_id::value_base_t(reg)});
                        if (range.first != range.second)
                            return w.region_membership_get_room(*range.first);
                    }
                    return a.room;
                };
                switch (e.key.key) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_R:
                    for (size_t i = 0; i < a.players.size(); ++i) {
                        player::reset(gs, a.players[i], a.room, a.spawn + vec2{float(i) * 20.f, 0.f});
                        player_graphics::reset(gs, a.players[i]);
                    }
                    break;
                case SDLK_PAGEDOWN: a.enter_room(room_at((uint32_t(a.room.index()) + 1) % n)); break;
                case SDLK_PAGEUP: a.enter_room(room_at((uint32_t(a.room.index()) + n - 1) % n)); break;
                case SDLK_END: a.enter_room(region_first(1)); break;
                case SDLK_HOME: a.enter_room(region_first(-1)); break;
                case SDLK_C: a.follow_camera = !a.follow_camera; break;
                case SDLK_F1: r.show_hud = !r.show_hud; break;
                case SDLK_F2: r.show_cameras = !r.show_cameras; break;
                case SDLK_G: r.show_geometry = !r.show_geometry; break;
                case SDLK_B: r.night_time = r.night_time > 0.5f ? 0.f : 1.f; break;
                case SDLK_F5:
                    // co-op slots are bounded by the player object's size in dcon
                    if (a.players.size() < 4) {
                        a.players.push_back(player::create(gs, a.room, a.spawn + vec2{float(a.players.size()) * 20.f, 0.f}, a.slugcat));
                        reports::add_log("player " + std::to_string(a.players.size()) + " joined");
                    }
                    break;
                case SDLK_P: paused = !paused; break;
                case SDLK_N: step_once = true; break;
                case SDLK_T: slow = !slow; break;
                case SDLK_1: case SDLK_2: case SDLK_3: {
                    a.slugcat = e.key.key == SDLK_1 ? player::slugcat_name::white
                        : e.key.key == SDLK_2 ? player::slugcat_name::yellow : player::slugcat_name::red;
                    // rebuild player one (body mass depends on the slugcat)
                    player::destroy(gs, a.players[0]);
                    a.players[0] = player::create(gs, a.room, a.spawn, a.slugcat);
                    break;
                }
                default: break;
                }
                break;
            }
            default:
                break;
            }
        }

        uint64_t const now = SDL_GetTicksNS();
        uint64_t const frame = now - last;
        last = now;
        accumulator += slow ? frame / 4 : frame;
        if (accumulator > tick_ns * 8)
            accumulator = tick_ns * 8; // don't spiral after a hitch
        if (paused) {
            accumulator = 0;
            if (step_once)
                tick();
        } else {
            while (accumulator >= tick_ns) {
                tick();
                accumulator -= tick_ns;
            }
        }
        step_once = false;

        a.update_camera(float(r.width) / a.cam.zoom, float(r.height) / a.cam.zoom);
        render::use_room(r, gs, a.room);
        float const alpha = paused ? 1.f : float(accumulator) / float(tick_ns);
        render::draw_frame(r, gs, a.view(), alpha, a.cam, a.hud(paused, slow));
        render::present(r);
    }

    if (pad != nullptr)
        SDL_CloseGamepad(pad);
    render::shutdown(r);
    return 0;
}
