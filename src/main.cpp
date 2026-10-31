#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "audio.hpp"
#include "centipede.hpp"
#include "leech.hpp"
#include "tube_worm.hpp"
#include "spawns.hpp"
#include "cicada.hpp"
#include "lizard.hpp"
#include "daddy.hpp"
#include "dropbug.hpp"
#include "config.h"
#include "gamestate.hpp"
#include "gates.hpp"
#include "physics.hpp"
#include "player.hpp"
#include "player_graphics.hpp"
#include "render.hpp"
#include "unity_assets.hpp"
#include "items.hpp"
#include "npc.hpp"
#include "rooms.hpp"
#include "rot.hpp"
#include "rain.hpp"
#include "dens.hpp"
#include "warps.hpp"
#include "reports.hpp"
#include "selftest.hpp"
#include "shortcuts.hpp"
#include "simulation.hpp"
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
            "  --slugcat <name>    survivor | monk | hunter | gourmand | artificer | rivulet |\n"
            "                      spearmaster | saint | sofanthiel | watcher (or white/yellow/red)\n"
            "  --selftest          run the scripted movement tests and exit\n"
            "  --fuzz [ticks]      soak test the slugcat in every room and exit\n"
            "  --bench [ticks]     time the whole world on one thread and on every thread, and\n"
            "                      exit: without creatures, one of each kind in every room, the\n"
            "                      world files' den population\n"
            "  --bench-mode <m>    only one of them: none, each, dens\n"
            "  --rain-in <ticks>   every region's rain comes in that many ticks\n"
            "  --no-dens           no world file creatures in their dens\n"
            "  --no-friendly-fire  a slugcat's thrown weapons pass through slugcats (NPCs included)\n"
            "  --npcs <share>      the share of shelters with a group of NPC slugcats (0..1, default 0.25)\n"
            "  --no-npcs           no NPC slugcats\n"
            "  --npc-trip <from> <to> [ticks]  one NPC slugcat (--slugcat, else random; --npc-seed) from\n"
            "                      shelter to shelter, no creatures, the rain frozen: pass or fail, then exit\n"
            "  --npc-seed <n>      the --npc-trip slugcat's rolls\n"
            "  --npc-human-complete <from> <to>  watch an NPC slugcat go from shelter to shelter (no creatures,\n"
            "                      the rain frozen); when it dies or gets stuck it is back at the pipe it came\n"
            "                      in by and you drive it, your input logged, until you enter the pipe it\n"
            "                      was heading for (then it exits; Tab held while watching: faster).\n"
            "                      --npc-human-log <file>: where the log goes; with --script, no window:\n"
            "                      the script replayed as what you'd do (does a recorded way work?)\n"
            "  NPC tracing and debugging (docs/SlugAI.md), with --npc-trip, --npc-soak or --selftest:\n"
            "  --npc-trace <room|*>[,every]  the NPC's state, input and move every few ticks in that room\n"
            "  --npc-route <r1,r2,...>  whether each room of a route can be walked from the way in to the way out\n"
            "  --npc-maps          with --npc-route, a reachability map of each room that can't\n"
            "  --npc-room <room>   dump a room's poles, pipes and walkable tiles (--npc-soak)\n"
            "  --npc-goal <exit>   with --npc-room, mark what reaches that exit; --npc-box <x0,y0,x1,y1>\n"
            "                      lists the walkable tiles there with their distance to it\n"
            "  --npc-edges <room,x0,y0,x1,y1>  the jumps leaving those tiles (--npc-soak)\n"
            "  --npc-dead-ends     the exits no other exit can be walked to from (--npc-soak)\n"
            "  --npc-unreachable   why the first NPCs with no shelter in reach have none (--npc-soak)\n"
            "  --npc-list          every NPC at the end of --npc-soak\n"
            "  --npc-guess         --npc-trip even when no walkable way is known (a guessed trip)\n"
            "  --npc-measure       with --selftest: the movement measurement instead of the tests\n"
            "  --npc-exp <room,x,y,mode,dir[,turn]>  with --npc-trip: a scripted jump off a pole\n"
            "  --npc-soak [ticks]  the world with its NPC slugcats (--npcs) for that many ticks, headless:\n"
            "                      how they get on and what they cost, then exit\n"
            "  --audio-check       load the sound effects, decode a song, report and exit\n"
            "  --audio-dump <wav>  no window: run --ticks ticks (and --script) mixing what is heard into a WAV,\n"
            "                      report its loudness and the loops' lifetimes\n"
            "  --screenshot <bmp>  render one frame after --ticks idle ticks, save it and exit\n"
            "  --ticks <n>         ticks to simulate before --screenshot (default 80)\n"
            "  --pos <x,y>         start position (feet, room pixels) instead of the spawn\n"
            "  --centipede <x,y[,size|variant]>  spawn a centipede of that size (0..1) or a\n"
            "                      variant: small (baby) wing (centiwing) red aqua (aquapede) (repeatable)\n"
            "  --dropbug <x,y>     spawn a dropwig, on the ceiling above if there is one (repeatable)\n"
            "  --cicada <x,y[,b]>  spawn a squidcada, white (CicadaA) or with b dark (CicadaB) (repeatable)\n"
            "  --lizard <x,y[,breed]>  spawn a lizard (default green): pink green blue yellow white\n"
            "                      red black salamander cyan caramel (spit) eel zoop train\n"
            "                      blizzard basilisk indigo peach (repeatable)\n"
            "  --daddy <x,y[,variant]>  spawn a daddy long legs (default daddy): daddy\n"
            "                      brother mother (TerrorLongLegs) hunter (repeatable)\n"
            "  --leech <x,y[,variant]>  spawn a leech (default the red one): sea jungle (repeatable)\n"
            "  --tubeworm <x,y>    spawn a grapple worm (repeatable)\n"
            "  --creature <kind,x,y>  spawn any creature by its world file name (repeatable):\n"
            "                      BigNeedleWorm SmallNeedleWorm PoleMimic TentaclePlant JetFish\n"
            "                      Scavenger Deer MirosBird, and the kinds above\n"
            "  --item <x,y,kind[,pearl]>  place an item (repeatable): rock spear explosive\n"
            "                      electric pearl (with a DataPearl type, e.g. cc) puffball\n"
            "  --weaver            the Watcher starts decorated\n"
            "  --script <s>        inputs before --screenshot, e.g. U2,N30,RJ12: letters\n"
            "                      U D L R J (jump) X (throw) P (pick up), N = nothing,\n"
            "                      held for the given ticks\n"
            "\n"
            "controls: arrows/WASD move, Z/Space jump, X throw, Left Shift pick up/drop\n"
            "          (hold down to drop), push into a pipe entrance to travel it,\n"
            "          R respawn, left click teleport, K spawn a centipede at the cursor\n"
            "          (Shift+K the next variant: centipede, baby, centiwing, red, aquapede;\n"
            "          Ctrl+K removes them all), O three leeches at the cursor (Shift+O the\n"
            "          next variant: leech, sea, jungle; Ctrl+O removes them all), V a dropwig on the\n"
            "          ceiling above the cursor (Ctrl+V removes them all), Q a white\n"
            "          squidcada at the cursor (Shift+Q a dark one, Ctrl+Q removes them all),\n"
            "          Y a lizard at the cursor, the next breed each time (Shift+Y the same\n"
            "          breed again, Ctrl+Y removes them all), H a daddy long legs at the\n"
            "          cursor (Shift+H the next variant, Ctrl+H removes them all), F a grapple\n"
            "          worm at the cursor (Ctrl+F removes them all), U a group of\n"
            "          three NPC slugcats at the cursor (Ctrl+U removes every NPC slugcat),\n"
            "          PageUp/PageDown room, Home/End region, C camera mode, F1 HUD, F2 camera outlines,\n"
            "          G collision geometry overlay, B day/night,\n"
            "          F5 add a co-op player (gamepad), P pause, N single step, T slow motion,\n"
            "          1-9/0 slugcat (Survivor, Monk, Hunter, Gourmand, Artificer, Rivulet,\n"
            "          Spearmaster, Saint, Sofanthiel, Watcher; 0 again toggles the Watcher's\n"
            "          Void Weaver decorations), F6 storm (rain streaks everywhere, sandstorms),\n"
            "          F7 the rain of the region in 30 seconds, M play a song of the region\n"
            "          (again for another one), Ctrl+M stop the music, Esc quit\n",
            argv0);
    }

    // a comma separated list
    std::vector<std::string> split(std::string const& s) {
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t const end = std::min(s.find(',', pos), s.size());
            if (end > pos)
                out.push_back(s.substr(pos, end - pos));
            pos = end + 1;
        }
        return out;
    }

    // the NPC slugcats' tracing and debugging options (npc::debug_options);
    // false: not one of them
    template<typename Next>
    bool npc_debug_option(std::string const& a, Next&& next) {
        auto& d = npc::debug();
        auto const ints = [](std::string const& s) {
            std::vector<int32_t> v;
            for (auto const& x : split(s))
                v.push_back(std::atoi(x.c_str()));
            return v;
        };
        if (a == "--npc-trace") {
            auto const parts = split(next());
            d.trace_room = parts.empty() ? "*" : parts[0];
            if (parts.size() > 1)
                d.trace_every = std::max(1, std::atoi(parts[1].c_str()));
        } else if (a == "--npc-route") {
            d.route = split(next());
        } else if (a == "--npc-maps") {
            d.maps = true;
        } else if (a == "--npc-room") {
            d.room = next();
        } else if (a == "--npc-goal") {
            d.goal = std::atoi(next().c_str());
        } else if (a == "--npc-box") {
            auto const v = ints(next());
            d.box = v.size() == 4;
            if (d.box) {
                d.box_lo = rw::ivec2{v[0], v[1]};
                d.box_hi = rw::ivec2{v[2], v[3]};
            }
        } else if (a == "--npc-edges") {
            auto const parts = split(next());
            if (parts.size() == 5) {
                d.edges_room = parts[0];
                d.edges_lo = rw::ivec2{std::atoi(parts[1].c_str()), std::atoi(parts[2].c_str())};
                d.edges_hi = rw::ivec2{std::atoi(parts[3].c_str()), std::atoi(parts[4].c_str())};
            }
        } else if (a == "--npc-dead-ends") {
            d.dead_ends = true;
        } else if (a == "--npc-unreachable") {
            d.unreachable = true;
        } else if (a == "--npc-list") {
            d.list = true;
        } else if (a == "--npc-guess") {
            d.guess = true;
        } else if (a == "--npc-measure") {
            d.measure = true;
        } else if (a == "--npc-exp") {
            d.experiment = next();
        } else {
            return false;
        }
        return true;
    }

    // --npc-human-complete: an NPC slugcat on its way; when it dies, falls
    // out of the room or gets stuck it is put back at the pipe it came in
    // by and a controller drives it, its input logged (in --script's form),
    // until it goes into the pipe the NPC would have left the room by
    struct human_complete {
        enum class state { watching, driving, done };
        bool on = false;
        dcon::player_id p;
        // where it came into the room it is in (the pipe's mouth; the spawn
        // in the first shelter)
        dcon::room_id entry_room;
        vec2 entry_pos{0.f, 0.f};
        rw::ivec2 entry_pipe{-1, -1};
        // the pipe (or warp) to leave by, the room behind it
        rw::ivec2 target{-1, -1};
        dcon::room_id target_room;
        bool driving = false;
        std::string log_path;
        std::string route;
        uint32_t no_way = 0;
        uint32_t stuck = 0;
        uint32_t attempts = 0;
        uint32_t ticks = 0;
        // the input so far: run lengths of letters
        std::vector<std::pair<std::string, uint32_t>> script;

        static std::string letters(player::input_package const& in) {
            std::string l;
            if (in.y > 0) l += 'U';
            if (in.y < 0) l += 'D';
            if (in.x < 0) l += 'L';
            if (in.x > 0) l += 'R';
            if (in.jmp) l += 'J';
            if (in.thrw) l += 'X';
            if (in.pckp) l += 'P';
            if (in.spec) l += 'E';
            return l.empty() ? "N" : l;
        }

        bool failed(sys::game_state& gs) const {
            auto const& w = gs.world;
            return player::dead(gs, p) || w.player_get_eaten(p)
                || (!player::in_shortcut(gs, p) && w.body_chunk_get_pos(player::body_chunk(gs, p, 1)).y < -40.f);
        }

        // back where it came into the room
        void respawn(sys::game_state& gs) {
            player::reset(gs, p, entry_room, entry_pos);
            player_graphics::reset(gs, p);
        }

        // the NPC failed: its turn is over
        void take_over(sys::game_state& gs, char const* why) {
            respawn(gs);
            target = npc::planned_exit(gs, p, target_room).value_or(rw::ivec2{-1, -1});
            npc::take_over(gs, p);
            driving = true;
            script.clear();
            ticks = 0;
            auto const& w = gs.world;
            std::string const room{gs.to_string_view(w.room_get_name(entry_room))};
            std::string const next = target_room ? std::string{gs.to_string_view(w.room_get_name(target_room))} : std::string{"?"};
            reports::add_log(std::string{"the NPC "} + why + " in " + room + ": your turn, into the pipe at " + std::to_string(target.x) + ","
                + std::to_string(target.y) + " (to " + next + ")");
        }

        void write_log(sys::game_state& gs) const {
            auto const& w = gs.world;
            std::FILE* f = std::fopen(log_path.c_str(), "w");
            if (!f) {
                reports::add_log("couldn't write " + log_path);
                return;
            }
            std::string const room{gs.to_string_view(w.room_get_name(entry_room))};
            std::fprintf(f, "# --npc-human-complete %s, %s\n", route.c_str(), player::to_string(w.player_get_stats(p).name));
            std::fprintf(f, "# room %s: in by the pipe at %d,%d (feet at %.1f,%.1f), out by the pipe at %d,%d to %s\n", room.c_str(), entry_pipe.x,
                entry_pipe.y, entry_pos.x, entry_pos.y, target.x, target.y,
                target_room ? std::string{gs.to_string_view(w.room_get_name(target_room))}.c_str() : "?");
            std::fprintf(f, "# attempts %u, %u ticks; the input as --script (U D L R J X P E, N nothing; then the ticks held):\n", attempts + 1, ticks);
            std::string line;
            for (size_t i = 0; i < script.size(); ++i)
                line += (i ? "," : "") + script[i].first + std::to_string(script[i].second);
            std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
            reports::add_log("wrote " + log_path);
        }

        state update(sys::game_state& gs, player::input_package const& in) {
            auto& w = gs.world;
            if (!driving) {
                simulation::tick(gs, {}, {});
                // into a new room: where it came in
                auto const r = rooms::room_of(gs, p);
                if (r && r != entry_room && !player::in_shortcut(gs, p)) {
                    entry_room = r;
                    vec2 const feet = w.body_chunk_get_pos(player::body_chunk(gs, p, 1));
                    entry_pos = feet;
                    float best = 1e9f;
                    w.room_for_each_shortcut_location(r, [&](dcon::shortcut_location_id sl) {
                        auto const sc = w.shortcut_location_get_shortcut(sl);
                        auto const t = w.shortcut_get_start(sc);
                        float const d = rw::distance(world::room_view::middle_of_tile(t), feet);
                        if (d < best) {
                            best = d;
                            entry_pipe = t;
                        }
                    });
                }
                if (failed(gs))
                    take_over(gs, "died");
                else if (npc::stats().stuck != stuck)
                    take_over(gs, "got stuck");
                // (with --npc-guess it plans by the rooms' connections where
                // it knows no walkable way: it takes over only once stuck)
                else if (npc::stats().no_way != no_way && !npc::debug().guess)
                    take_over(gs, "found no way");
                else if (r == gs.find_room(route.substr(route.find(" -> ") + 4)) && w.slugcat_npc_get_nav(npc::of(gs, p)).what == npc::goal::rest) {
                    reports::add_log("the NPC got there on its own");
                    return state::done;
                }
                return state::watching;
            }
            // driving: logged, until it is in the pipe (or through the warp)
            auto const l = letters(in);
            if (!script.empty() && script.back().first == l)
                ++script.back().second;
            else
                script.emplace_back(l, 1u);
            ++ticks;
            dcon::player_id const ps[1] = {p};
            simulation::tick(gs, ps, std::span{&in, 1});
            // --npc-trace: what the human does
            if (auto const& d = npc::debug(); !d.trace_room.empty() && ticks % uint32_t(std::max(1, d.trace_every)) == 0) {
                vec2 const h = w.body_chunk_get_pos(player::body_chunk(gs, p, 0)), f = w.body_chunk_get_pos(player::body_chunk(gs, p, 1));
                auto const ht = world::room_view::get_tile_position(h), ft = world::room_view::get_tile_position(f);
                std::printf("    human t=%u %-4s head (%d,%d) feet (%d,%d) %.0f,%.0f mode %s anim %s\n", ticks, l.c_str(), ht.x, ht.y, ft.x, ft.y, f.x, f.y,
                    player::to_string(w.player_get_body_mode(p)), player::to_string(w.player_get_animation(p)));
            }
            if (failed(gs)) {
                ++attempts;
                script.clear();
                ticks = 0;
                respawn(gs);
                reports::add_log("died: again from the start of the room (attempt " + std::to_string(attempts + 1) + ")");
                return state::driving;
            }
            bool const piped = player::in_shortcut(gs, p) && w.shortcut_get_start(w.creature_get_vessel_shortcut(player::creature_of(gs, p))) == target;
            if (piped || (target_room && rooms::room_of(gs, p) == target_room)) {
                write_log(gs);
                return state::done;
            }
            return state::driving;
        }
    };

    player::slugcat_name parse_slugcat(std::string n) {
        for (auto& c : n)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (n == "white") return player::slugcat_name::white;
        if (n == "yellow") return player::slugcat_name::yellow;
        if (n == "red") return player::slugcat_name::red;
        if (n == "inv") return player::slugcat_name::inv;
        for (uint32_t i = 0; i < uint32_t(player::slugcat_name::count); ++i) {
            std::string name = player::to_string(player::slugcat_name(i));
            for (auto& c : name)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            if (name == n)
                return player::slugcat_name(i);
        }
        return player::slugcat_name::white;
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
            log_room();
        }

        // the view follows player one through pipes and gates
        void follow_player() {
            auto const pr = simulation::player_room(gs, players[0]);
            if (pr && pr != room) {
                room = pr;
                camera_index = 0;
                auto const cams = camera_positions();
                vec2 const first = cams.empty() ? vec2{0.f, 0.f} : cams[0];
                spawn = selftest::find_spawn(view(), first + vec2{gs.screen_width / 2.f, gs.screen_height / 2.f});
                log_room();
            }
        }

        void log_room() const {
            auto const rid = room;
            auto const& w = gs.world;
            reports::add_log("room " + std::string{gs.to_string_view(w.room_get_display_name(rid))} + " ("
                + std::string{gs.to_string_view(w.region_get_name(w.room_get_region_from_region_membership(rid)))} + ") "
                + std::to_string(w.room_get_width(rid)) + "x" + std::to_string(w.room_get_height(rid)));
            std::string pipes;
            w.room_for_each_shortcut_location(rid, [&](dcon::shortcut_location_id sl) {
                auto const sc = w.shortcut_location_get_shortcut(sl);
                auto const exit = w.shortcut_get_exit_from_shortcut_destination(sc);
                dcon::room_id const target = exit ? dcon::room_id{w.room_exit_get_target_from_exit_connection(exit)} : dcon::room_id{};
                if (w.shortcut_get_type(sc) != world::shortcut_type::room_exit && w.shortcut_get_type(sc) != world::shortcut_type::normal)
                    return;
                rw::ivec2 const t = w.shortcut_get_start(sc);
                pipes += " (" + std::to_string(t.x) + "," + std::to_string(t.y) + ")->"
                    + (target ? std::string{gs.to_string_view(w.room_get_name(target))} : std::string{"room"});
            });
            if (!pipes.empty())
                reports::add_log("  pipes:" + pipes);
            uint32_t rocks = 0;
            w.for_each_item([&](dcon::item_id it) {
                if (w.physical_object_get_room_from_object_location(w.item_get_body_from_item_body(it)) == rid)
                    ++rocks;
            });
            if (rocks > 0)
                reports::add_log("  rocks: " + std::to_string(rocks));
        }

        // Snap to the vanilla camera containing player one, like RoomCamera
        // VirtualMicrophone.listenerPoint: the followed slugcat, or the
        // middle of the view when it is elsewhere
        vec2 listener(vec2 view_center) const {
            if (player::in_shortcut(gs, players[0]))
                return shortcuts::vessel_position(gs, players[0], 1.f);
            if (simulation::player_room(gs, players[0]) != room)
                return view_center;
            return gs.world.body_chunk_get_pos(player::body_chunk(gs, players[0], 0));
        }

        void update_camera(float view_w, float view_h) {
            vec2 const head = player::in_shortcut(gs, players[0]) ? shortcuts::vessel_position(gs, players[0], 1.f)
                : gs.world.body_chunk_get_pos(player::body_chunk(gs, players[0], 0));
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
            uint32_t rocks = 0, spears = 0, stuck = 0;
            w.for_each_item([&](dcon::item_id it) {
                if (w.physical_object_get_room_from_object_location(w.item_get_body_from_item_body(it)) != room)
                    return;
                bool const spear = w.item_get_type(it) == world::item_type::spear;
                ++(spear ? spears : rocks);
                stuck += w.item_get_mode(it) == world::weapon_mode::stuck_in_wall;
            });
            std::string held;
            for (uint32_t hand = 0; hand < 2; ++hand)
                if (auto const it = items::held(gs, p, hand))
                    held += w.item_get_type(it) == world::item_type::spear ? " spear" : " rock";
            if (cicada::held_by(gs, p))
                held += " squidcada";
            if (held.empty())
                held = " nothing";
            char buf[896];
            std::snprintf(buf, sizeof(buf),
                "%s  [%s]%s%s  %s\n"
                "mode %s  anim %s\n"
                "vel %.2f %.2f  feet %.0f %.0f\n"
                "contact %d %d / %d %d  slope %d\n"
                "standing %d canJump %d wantJump %d stun %d\n"
                "air %.2f  aerobic %.2f  players %zu%s\n"
                "rocks %u  spears %u (%u in walls)  holding%s%s",
                std::string{gs.to_string_view(w.room_get_display_name(room))}.c_str(),
                std::string{gs.to_string_view(w.region_get_display_name(w.room_get_region_from_region_membership(room)))}.c_str(),
                paused ? "  PAUSED" : "", slow ? "  SLOW" : "", player::to_string(w.player_get_stats(p).name),
                player::to_string(w.player_get_body_mode(p)), player::to_string(w.player_get_animation(p)),
                w.body_chunk_get_vel(c0).x, w.body_chunk_get_vel(c0).y, w.body_chunk_get_pos(c1).x, w.body_chunk_get_pos(c1).y,
                w.body_chunk_get_contact_point(c0).x, w.body_chunk_get_contact_point(c0).y,
                w.body_chunk_get_contact_point(c1).x, w.body_chunk_get_contact_point(c1).y, int(w.body_chunk_get_on_slope(c1)),
                int(w.player_get_standing(p)), w.player_get_can_jump(p), w.player_get_want_to_jump(p), player::stun_of(gs, p),
                w.player_get_air_in_lungs(p), w.player_get_aerobic_level(p), players.size(), player::dead(gs, p) ? "  DEAD (R)" : "",
                rocks, spears, stuck, held.c_str(), player::in_shortcut(gs, p) ? "  IN PIPE" : "");
            std::string out = buf;
            if (auto const* rc = rain::of_room(gs, room)) {
                // the region's rain cycle (RainMeter) and this room's rain
                char rb[200];
                int32_t const left = rain::time_until_rain(*rc);
                if (rain::deadly(*rc))
                    std::snprintf(rb, sizeof(rb), "\nDEATH RAIN %d/%d  intensity %.2f  room %.2f  flood %.0f", rc->death_rain_ticks, rain::deadly_rain_ticks,
                        rc->intensity, w.room_get_rain_now(room), rc->flood);
                else
                    std::snprintf(rb, sizeof(rb), "\nrain in %d:%02d (cycle %d s)  intensity %.2f  room %.2f%s", left / 40 / 60, left / 40 % 60,
                        rc->cycle_length / 40, rc->intensity, w.room_get_rain_now(room), w.region_get_never_rains(w.room_get_rain_region(room)) ? "  (never rains)" : "");
                out += rb;
            }
            if (auto const g = gates::gate_of(gs, room)) {
                static char const* const modes[] = {"MiddleClosed", "ClosingAirLock", "Waiting", "OpeningMiddle",
                    "MiddleOpen", "ClosingMiddle", "OpeningSide", "Closed", "Broken"};
                rw::vec3 const d = w.gate_get_door_closed(g);
                char gb[160];
                std::snprintf(gb, sizeof(gb), "\ngate %s  doors %.2f %.2f %.2f  still %d",
                    modes[size_t(w.gate_get_mode(g))], d[0], d[1], d[2], w.player_get_touched_no_input_counter(p));
                out += gb;
            }
            return out;
        }
    };

    player::input_package keyboard_input() {
        bool const* keys = SDL_GetKeyboardState(nullptr);
        float ax = 0.f, ay = 0.f;
        if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A]) ax -= 1.f;
        if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) ax += 1.f;
        if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S]) ay -= 1.f;
        if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W]) ay += 1.f;
        bool const jmp = keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_SPACE];
        bool const thrw = keys[SDL_SCANCODE_X] || keys[SDL_SCANCODE_J];
        bool const pckp = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_L];
        auto in = player::make_input(ax, ay, jmp, thrw, pckp);
        // the Watcher's ability (camouflage, levitation)
        in.spec = keys[SDL_SCANCODE_E];
        return in;
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
        auto in = player::make_input(ax, ay, SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH),
            SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST), SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
        in.spec = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_NORTH);
        in.gamepad = true;
        return in;
    }
}

int main(int argc, char* argv[]) {
    std::vector<std::string> roots;
    // the game's Unity data (RainWorld_Data: resources.assets), for its sprites
    std::string unity_dir;
    std::string start_room = "su_a22";
    std::string screenshot;
    uint32_t screenshot_ticks = 80;
    std::string script;
    std::optional<vec2> start_pos;
    // the creatures to spawn, made kind by kind (creature::type order)
    std::vector<std::pair<vec2, creature::spawn_desc>> spawn_creatures;
    struct item_spawn {
        vec2 at;
        world::item_type type;
        std::string pearl;
    };
    std::vector<item_spawn> spawn_items;
    bool weaver = false;
    bool selftest_mode = false;
    bool audio_check = false;
    std::string audio_dump;
    int32_t fuzz_ticks = -1;
    int32_t bench_ticks = -1;
    int32_t npc_soak_ticks = -1;
    std::string npc_trip_from, npc_trip_to;
    std::string human_from, human_to, human_log;
    uint32_t npc_trip_ticks = 40 * 60 * 10;
    uint32_t npc_seed = 1;
    bool slugcat_given = false;
    std::string bench_mode;
    int32_t rain_in = -1;
    bool no_dens = false;
    bool friendly_fire = true;
    float npc_chance = npc::default_shelter_chance;
    player::slugcat_name slugcat = player::slugcat_name::white;
    for (int i = 1; i < argc; ++i) {
        std::string const a = argv[i];
        auto const next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--root") {
            roots.push_back(next());
        } else if (a == "--unity") {
            unity_dir = next();
        } else if (a == "--room") {
            start_room = next();
        } else if (a == "--slugcat") {
            slugcat = parse_slugcat(next());
            slugcat_given = true;
        } else if (a == "--audio-check") {
            audio_check = true;
        } else if (a == "--audio-dump") {
            audio_dump = next();
        } else if (a == "--selftest") {
            selftest_mode = true;
        } else if (a == "--fuzz") {
            fuzz_ticks = 400;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                fuzz_ticks = std::atoi(argv[++i]);
        } else if (a == "--bench") {
            bench_ticks = 200;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                bench_ticks = std::atoi(argv[++i]);
        } else if (a == "--npc-trip") {
            npc_trip_from = next();
            npc_trip_to = next();
            if (i + 1 < argc && argv[i + 1][0] != '-')
                npc_trip_ticks = uint32_t(std::atoi(argv[++i]));
        } else if (a.starts_with("--npc-") && npc_debug_option(a, next)) {
        } else if (a == "--npc-human-complete") {
            human_from = next();
            human_to = next();
        } else if (a == "--npc-human-log") {
            human_log = next();
        } else if (a == "--npc-seed") {
            npc_seed = uint32_t(std::atoi(next().c_str()));
        } else if (a == "--npc-soak") {
            npc_soak_ticks = 6000;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                npc_soak_ticks = std::atoi(argv[++i]);
        } else if (a == "--bench-mode") {
            bench_mode = next();
        } else if (a == "--rain-in") {
            rain_in = std::atoi(next().c_str());
        } else if (a == "--no-dens") {
            no_dens = true;
        } else if (a == "--no-friendly-fire") {
            friendly_fire = false;
        } else if (a == "--npcs") {
            npc_chance = float(std::atof(next().c_str()));
        } else if (a == "--no-npcs") {
            npc_chance = 0.f;
        } else if (a == "--screenshot") {
            screenshot = next();
        } else if (a == "--script") {
            script = next();
        } else if (a == "--pos") {
            float x = 0.f, y = 0.f;
            if (std::sscanf(next().c_str(), "%f,%f", &x, &y) == 2)
                start_pos = vec2{x, y};
        } else if (a == "--weaver") {
            weaver = true;
        } else if (a == "--centipede") {
            float x = 0.f, y = 0.f, sz = -1.f;
            char name[32] = "";
            if (std::sscanf(next().c_str(), "%f,%f,%31s", &x, &y, name) >= 2) {
                auto v = centipede::variant::centipede;
                char* end = nullptr;
                float const f = std::strtof(name, &end);
                if (name[0] != 0 && end != nullptr && *end == 0)
                    sz = f;
                else if (name[0] != 0 && !centipede::parse_variant(name, v))
                    std::fprintf(stderr, "unknown centipede %s, a centipede instead\n", name);
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::centipede, sz, false, uint8_t(v)}});
            }
        } else if (a == "--leech") {
            float x = 0.f, y = 0.f;
            char name[32] = "leech";
            auto v = leech::variant::leech;
            if (std::sscanf(next().c_str(), "%f,%f,%31s", &x, &y, name) >= 2) {
                if (!leech::parse_variant(name, v))
                    std::fprintf(stderr, "unknown leech %s, a leech instead\n", name);
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::leech, -1.f, false, uint8_t(v)}});
            }
        } else if (a == "--creature") {
            std::string const arg = next();
            float x = 0.f, y = 0.f;
            auto const comma = arg.find(',');
            auto const k = comma == std::string::npos ? std::nullopt : spawns::kind_of(std::string_view{arg}.substr(0, comma));
            if (k && std::sscanf(arg.c_str() + comma + 1, "%f,%f", &x, &y) == 2)
                spawn_creatures.push_back({vec2{x, y}, *k});
            else
                std::fprintf(stderr, "--creature %s: unknown kind or position\n", arg.c_str());
        } else if (a == "--tubeworm") {
            float x = 0.f, y = 0.f;
            if (std::sscanf(next().c_str(), "%f,%f", &x, &y) == 2)
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::tube_worm}});
        } else if (a == "--dropbug") {
            float x = 0.f, y = 0.f;
            if (std::sscanf(next().c_str(), "%f,%f", &x, &y) == 2)
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::dropbug}});
        } else if (a == "--cicada") {
            float x = 0.f, y = 0.f;
            char kind = 'a';
            if (std::sscanf(next().c_str(), "%f,%f,%c", &x, &y, &kind) >= 2)
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::cicada, kind == 'b' ? 0.f : 1.f}});
        } else if (a == "--lizard") {
            float x = 0.f, y = 0.f;
            char name[32] = "green";
            auto b = lizard::breed::green;
            if (std::sscanf(next().c_str(), "%f,%f,%31s", &x, &y, name) >= 2) {
                if (!lizard::parse_breed(name, b))
                    std::fprintf(stderr, "unknown lizard breed %s, a green one instead\n", name);
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::lizard, -1.f, false, uint8_t(b)}});
            }
        } else if (a == "--daddy") {
            float x = 0.f, y = 0.f;
            char name[32] = "daddy";
            auto v = daddy::variant::daddy;
            if (std::sscanf(next().c_str(), "%f,%f,%31s", &x, &y, name) >= 2) {
                if (!daddy::parse_variant(name, v))
                    std::fprintf(stderr, "unknown daddy long legs %s, a daddy instead\n", name);
                spawn_creatures.push_back({vec2{x, y}, creature::spawn_desc{creature::type::daddy, -1.f, false, uint8_t(v)}});
            }
        } else if (a == "--item") {
            float x = 0.f, y = 0.f;
            char kind[64] = "rock";
            if (std::sscanf(next().c_str(), "%f,%f,%63s", &x, &y, kind) >= 2) {
                std::string k = kind, pearl;
                if (auto const comma = k.find(','); comma != std::string::npos) {
                    pearl = k.substr(comma + 1);
                    k.resize(comma);
                }
                auto t = world::item_type::rock;
                if (k == "spear") t = world::item_type::spear;
                else if (k == "explosive") t = world::item_type::explosive_spear;
                else if (k == "electric") t = world::item_type::electric_spear;
                else if (k == "pearl") t = world::item_type::data_pearl;
                else if (k == "puffball") t = world::item_type::puff_ball;
                else if (k != "rock") std::fprintf(stderr, "unknown item %s, a rock instead\n", kind);
                spawn_items.push_back({vec2{x, y}, t, pearl});
            }
        } else if (a == "--ticks") {
            screenshot_ticks = uint32_t(std::atoi(next().c_str()));
        } else {
            print_usage(argv[0]);
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }

    if (selftest_mode)
        return selftest::run();

    if (roots.empty()) {
        auto const sa = find_streaming_assets();
        if (sa.empty()) {
            reports::add_log("can't find Rain World's StreamingAssets, pass --root or set RW_ROOT");
            return 1;
        }
        roots.push_back(sa);
    }
    auto const load_world = [&](bool log) {
        auto ptr = sys::make_game_state();
        auto& g = *ptr;
        // our own assets (HUD font) come before so the game data can be overriden
        if (char const* base = SDL_GetBasePath(); base != nullptr)
            g.fs.add_root(std::string{base} + "assets");
        for (auto const& r : roots) {
            if (log)
                reports::add_log("root: " + r);
            g.fs.add_root(r);
        }
        g.fs.generate_unpopulated_tree();
        parsers::error_handler err;
        world::load(g, err);
        if (log && !err.accumulated_errors.empty())
            reports::add_log("errors:\n" + err.accumulated_errors);
        if (log && !err.accumulated_warnings.empty())
            reports::add_log("warnings:\n" + err.accumulated_warnings);
        if (err.fatal || g.world.room_size() == 0)
            reports::error_fatal("no rooms loaded");
        return ptr;
    };
    if (bench_ticks >= 0) {
        bool first = true;
        return selftest::bench([&] {
            auto ptr = load_world(first);
            ptr->story_slugcat = slugcat;
            first = false;
            return ptr;
        }, uint32_t(bench_ticks), bench_mode);
    }
    auto const gs_ptr = load_world(true);
    auto& gs = *gs_ptr;
    gs.spawn_dens = !no_dens;
    gs.friendly_fire = friendly_fire;
    // (not for the fuzz: it drives slugcats of its own)
    gs.npc_shelter_chance = fuzz_ticks >= 0 ? 0.f : npc_chance;
    // --npc-human-complete: the trip test's world (no creatures, no other
    // NPCs, the rain standing still)
    bool const human_run = !human_from.empty();
    if (human_run) {
        gs.spawn_dens = false;
        gs.npc_shelter_chance = 0.f;
        gs.rain_frozen = true;
        start_room = human_from;
    }
    // the campaign: what the rooms spawn (and the pearls' tint) follows its timeline
    gs.story_slugcat = slugcat;

    if (!npc_trip_from.empty())
        return selftest::npc_trip(gs, npc_trip_from, npc_trip_to, npc_trip_ticks, slugcat_given ? slugcat : player::slugcat_name::count, npc_seed);
    if (npc_soak_ticks >= 0)
        return selftest::npc_soak(gs, uint32_t(npc_soak_ticks));
    if (fuzz_ticks >= 0) {
        simulation::populate(gs);
        return selftest::fuzz(gs, uint32_t(fuzz_ticks));
    }
    if (audio_check) {
        auto sound = audio::create(gs);
        // the check needs no device
        if (!sound)
            sound = audio::create_offline(gs);
        return sound && audio::self_check(*sound, gs) ? 0 : 1;
    }

    // vanilla rolls item spawns with UnityEngine.Random; screenshots stay reproducible
    if (screenshot.empty() && audio_dump.empty())
        gs.random.state ^= uint32_t(SDL_GetTicksNS() * 2654435761ull) | 1u;
    app a{gs};
    a.slugcat = slugcat;
    // every room's first visit, now: items, scavenger loot, rot, warps,
    // sand, the rain cycles and the creatures in their dens
    simulation::populate(gs);
    if (rain_in >= 0)
        gs.world.for_each_region([&](dcon::region_id r) { rain::rain_in(gs, r, rain_in); });
    dcon::room_id start = gs.find_room(start_room);
    if (!start) {
        reports::add_log("unknown room \"" + start_room + "\", using the first one");
        start = dcon::room_id{0};
    }
    a.players.push_back(player::create(gs, start, vec2{0.f, 0.f}, slugcat));
    a.enter_room(start);
    if (weaver)
        player_graphics::set_weaver(gs, a.players[0], true);
    std::stable_sort(spawn_creatures.begin(), spawn_creatures.end(), [](auto const& x, auto const& y) { return x.second.kind < y.second.kind; });
    for (auto const& [at, d] : spawn_creatures)
        creature::create(gs, start, at, d);
    for (auto const& i : spawn_items) {
        auto const it = items::create(gs, start, i.at, i.type);
        if (!i.pearl.empty())
            items::set_pearl_type(gs, it, i.pearl);
    }
    if (start_pos) {
        a.spawn = *start_pos;
        player::reset(gs, a.players[0], start, *start_pos);
        player_graphics::reset(gs, a.players[0]);
    }
    // --npc-human-complete: the NPC slugcat is player one (the camera
    // follows it); a controller drives it once it fails
    human_complete hc;
    if (human_run) {
        auto const to = gs.find_room(human_to);
        if (!to || !gs.world.room_get_is_shelter(start) || !gs.world.room_get_is_shelter(to)) {
            std::printf("--npc-human-complete: %s and %s must both be shelters\n", human_from.c_str(), human_to.c_str());
            return 1;
        }
        player::destroy(gs, a.players[0]);
        rw::rng rng{npc_seed | 1u};
        a.players[0] = npc::create(gs, start, a.spawn, rng, {}, slugcat_given ? slugcat : player::slugcat_name::count);
        auto const n = npc::of(gs, a.players[0]);
        gs.world.slugcat_npc_set_target(n, to);
        gs.world.slugcat_npc_get_nav(n).goal_timer = 0;
        hc.on = true;
        hc.p = a.players[0];
        hc.entry_room = start;
        hc.entry_pos = a.spawn;
        hc.log_path = human_log.empty() ? "npc_human_" + human_from + "_" + human_to + ".txt" : human_log;
        hc.route = human_from + " -> " + human_to;
        hc.no_way = npc::stats().no_way;
        hc.stuck = npc::stats().stuck;
        reports::add_log("watching " + npc::describe(gs, hc.p) + "; when it fails you take over");
    }

    // scripted input: comma separated "<letters><ticks>"
    auto const run_script = [&](auto&& step) {
        size_t pos = 0;
        while (pos < script.size()) {
            size_t const end = std::min(script.find(',', pos), script.size());
            std::string const tok = script.substr(pos, end - pos);
            pos = end + 1;
            float ax = 0.f, ay = 0.f;
            bool jmp = false, thrw = false, pckp = false, spec = false;
            size_t i = 0;
            for (; i < tok.size() && std::isalpha(static_cast<unsigned char>(tok[i])); ++i) {
                switch (tok[i]) {
                case 'U': ay += 1.f; break;
                case 'D': ay -= 1.f; break;
                case 'L': ax -= 1.f; break;
                case 'R': ax += 1.f; break;
                case 'J': jmp = true; break;
                case 'X': thrw = true; break;
                case 'P': pckp = true; break;
                case 'E': spec = true; break;
                default: break;
                }
            }
            int const n = i < tok.size() ? std::atoi(tok.c_str() + i) : 1;
            for (int k = 0; k < n; ++k) {
                auto in = player::make_input(ax, ay, jmp, thrw, pckp);
                in.spec = spec;
                step(in);
            }
        }
    };

    // --npc-human-complete with --script: no window, the script is what the
    // human does once the NPC fails (a recorded log replayed: it tells
    // whether the way recorded works)
    if (hc.on && !script.empty()) {
        std::vector<player::input_package> replay;
        run_script([&](player::input_package const& in) { replay.push_back(in); });
        size_t k = 0;
        for (uint32_t t = 0; t < 40u * 60u * 30u; ++t) {
            bool const was_driving = hc.driving;
            uint32_t const attempts = hc.attempts;
            player::input_package const in = was_driving && k < replay.size() ? replay[k++] : player::input_package{};
            auto const st = hc.update(gs, in);
            gs.sounds.clear();
            gs.music.clear();
            if (st == human_complete::state::done) {
                std::printf("PASS after %u ticks\n", t);
                return 0;
            }
            // died: the script again from its start
            if (hc.attempts != attempts)
                k = 0;
            if (was_driving && k >= replay.size() && hc.ticks > replay.size() + 200) {
                std::printf("FAIL: the script ran out %u ticks ago\n", hc.ticks - uint32_t(replay.size()));
                return 1;
            }
        }
        std::printf("FAIL: out of time\n");
        return 1;
    }

    // --audio-dump: the headless run, heard instead of seen
    if (!audio_dump.empty()) {
        auto sound = audio::create_offline(gs);
        if (!sound)
            return 1;
        vec2 const size{gs.screen_width, gs.screen_height};
        auto const step = [&](player::input_package const& in) {
            gs.camera_rooms.assign(1, a.room);
            simulation::tick(gs, a.players, std::span{&in, 1});
            a.follow_player();
            a.update_camera(size.x, size.y);
            vec2 const center = a.cam.pos + size * 0.5f;
            audio::update(*sound, gs, a.room, center, size, a.listener(center), 1.f / float(gs.ticks_per_second));
        };
        for (uint32_t t = 0; t < screenshot_ticks; ++t)
            step(player::input_package{});
        run_script(step);
        return audio::dump(*sound, audio_dump.c_str()) ? 0 : 1;
    }

    render::state r;
    bool const headless = !screenshot.empty();
    if (!render::init(r, int(gs.screen_width), int(gs.screen_height), "rwcpp", headless))
        return 1;
    r.unity_dir = unity::find_asset_dir(roots, unity_dir);
    render::load_assets(r, gs);

    if (headless) {
        auto const step = [&](player::input_package const& in) {
            gs.camera_rooms.assign(1, a.room);
            simulation::tick(gs, a.players, std::span{&in, 1});
            a.follow_player();
            gs.sounds.clear(); // nothing plays them headless
            gs.music.clear();
        };
        for (uint32_t t = 0; t < screenshot_ticks; ++t)
            step(player::input_package{});
        run_script(step);
        // the creatures of the room the camera shows
        for (auto const t : creature::all_types)
            creature::for_each_of(gs.world, t, [&](dcon::creature_id c) {
                if (creature::room_of(gs, c) == a.room || (dens::where_is(gs, c) != dens::where::room && gs.world.creature_get_away_room(c) == a.room))
                    reports::add_log(creature::describe(gs, c));
            });
        if (auto const rs = rot::system_of(gs, a.room); rs) {
            auto const& w = gs.world;
            uint32_t bulbs = 0, eyes = 0, legs = 0, tendrils = 0, vines = 0, meals = 0;
            w.rot_system_for_each_rot_bulb_membership(rs, [&](dcon::rot_bulb_membership_id m) {
                ++bulbs;
                eyes += w.rot_bulb_get_has_eye(w.rot_bulb_membership_get_bulb(m)) ? 1 : 0;
            });
            w.rot_system_for_each_rot_tube_membership(rs, [&](dcon::rot_tube_membership_id m) {
                auto const k = w.rot_tube_get_kind(w.rot_tube_membership_get_tube(m));
                (k == world::rot_tube_kind::leg ? legs : k == world::rot_tube_kind::tendril ? tendrils : vines) += 1;
            });
            w.rot_system_for_each_rot_meal_membership(rs, [&](dcon::rot_meal_membership_id) { ++meals; });
            reports::add_log("rot: " + std::to_string(bulbs) + " bulbs (" + std::to_string(eyes) + " eyes), " + std::to_string(legs) + " legs, "
                + std::to_string(tendrils) + " tendrils, " + std::to_string(vines) + " vines, " + std::to_string(meals) + " meals"
                + (w.player_get_eaten(a.players[0]) ? ", the slugcat was eaten" : ""));
        }
        {
            auto const& w = gs.world;
            std::string line = "warps: ripple " + std::to_string(gs.ripple_level).substr(0, 3) + " layer " + std::to_string(warps::active_ripple_layer(gs));
            if (gs.warp.active)
                line += ", warping " + std::to_string(int(gs.warp.progress)) + "/" + std::to_string(int(gs.warp.duration));
            w.room_for_each_warp_point_location(a.room, [&](dcon::warp_point_location_id l) {
                auto const wp = w.warp_point_location_get_point(l);
                line += ", state " + std::to_string(int(w.warp_point_get_state(wp))) + " open " + std::to_string(w.warp_point_get_open(wp)).substr(0, 4)
                    + " fade " + std::to_string(w.warp_point_get_fade(wp)).substr(0, 4);
            });
            reports::add_log(line);
        }
        render::use_room(r, gs, a.room);
        a.update_camera(float(r.width), float(r.height));
        render::draw_frame(r, gs, a.view(), 1.f, a.cam, a.hud(false, false));
        bool const ok = render::save_screenshot(r, screenshot.c_str());
        render::shutdown(r);
        reports::add_log(ok ? "saved " + screenshot : std::string{"screenshot failed"});
        return ok ? 0 : 1;
    }

    auto sound = audio::create(gs);
    SDL_Gamepad* pad = nullptr;
    bool running = true;
    bool paused = false;
    bool slow = false;
    bool step_once = false;
    uint64_t const tick_ns = 1000000000ull / gs.ticks_per_second;
    uint64_t last = SDL_GetTicksNS();
    uint64_t accumulator = 0;
    std::vector<player::input_package> inputs;
    auto const tick = [&] {
        if (hc.on) {
            auto in = keyboard_input();
            if (auto const g = gamepad_input(pad); g.analogue_dir != vec2{0.f, 0.f} || g.jmp || g.thrw || g.pckp)
                in = g;
            gs.camera_rooms.assign(1, a.room);
            // (watching, Tab held: four times as fast)
            int const steps = !hc.driving && SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_TAB] ? 4 : 1;
            for (int i = 0; i < steps && running; ++i)
                if (hc.update(gs, in) == human_complete::state::done)
                    running = false;
            a.follow_player();
            return;
        }
        inputs.clear();
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
            inputs.push_back(in);
        }
        gs.camera_rooms.assign(1, a.room);
        simulation::tick(gs, a.players, inputs);
        a.follow_player();
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
                // Ctrl + a creature's key: every creature of that kind goes
                auto const remove_all = [&](creature::type t) {
                    std::vector<dcon::creature_id> all;
                    creature::for_each_of(w, t, [&](dcon::creature_id c) { all.push_back(c); });
                    for (auto const c : all)
                        creature::destroy(gs, c);
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
                case SDLK_K: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::centipede);
                        break;
                    }
                    // Shift: on to the next variant
                    static uint32_t variant = 0;
                    if ((e.key.mod & SDL_KMOD_SHIFT) != 0)
                        variant = (variant + 1) % centipede::variant_count;
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    // same mapping as the teleport click
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    auto const c = centipede::create(gs, a.room, at, -1.f, centipede::variant(variant));
                    reports::add_log(std::string{centipede::to_string(centipede::variant(variant))} + ", size "
                        + std::to_string(gs.world.creature_get_payload(c).centipede.size));
                    break;
                }
                case SDLK_O: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::leech);
                        break;
                    }
                    // Shift: on to the next variant; a little school each time
                    static uint32_t variant = 0;
                    if ((e.key.mod & SDL_KMOD_SHIFT) != 0)
                        variant = (variant + 1) % leech::variant_count;
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    for (int i = 0; i < 3; ++i)
                        leech::create(gs, a.room, at + vec2{float(i - 1) * 8.f, 0.f}, leech::variant(variant));
                    reports::add_log(std::string{"3 x "} + leech::to_string(leech::variant(variant)));
                    break;
                }
                case SDLK_F: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::tube_worm);
                        break;
                    }
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    tube_worm::create(gs, a.room, at);
                    reports::add_log("grapple worm");
                    break;
                }
                case SDLK_V: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::dropbug);
                        break;
                    }
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    auto const d = dropbug::create(gs, a.room, at);
                    reports::add_log(std::string{"dropwig"} + (dropbug::sitting_in_ceiling(gs, d) ? " in the ceiling" : ""));
                    break;
                }
                case SDLK_Q: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::cicada);
                        break;
                    }
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    bool const white = (e.key.mod & SDL_KMOD_SHIFT) == 0;
                    cicada::create(gs, a.room, at, white);
                    reports::add_log(white ? "white squidcada" : "dark squidcada");
                    break;
                }
                case SDLK_Y: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::lizard);
                        break;
                    }
                    // the next breed each time (Shift: the last one again)
                    static uint32_t next_breed = 0;
                    if ((e.key.mod & SDL_KMOD_SHIFT) != 0 && next_breed > 0)
                        --next_breed;
                    auto const b = lizard::breed(next_breed % lizard::breed_count);
                    next_breed = next_breed % lizard::breed_count + 1;
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    lizard::create(gs, a.room, render::screen_to_room(r, a.cam, mx, my), b);
                    reports::add_log(std::string{lizard::to_string(b)} + " lizard");
                    break;
                }
                case SDLK_H: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        remove_all(creature::type::daddy);
                        break;
                    }
                    // Shift: on to the next variant
                    static uint32_t variant = 0;
                    if ((e.key.mod & SDL_KMOD_SHIFT) != 0)
                        variant = (variant + 1) % daddy::variant_count;
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    daddy::create(gs, a.room, render::screen_to_room(r, a.cam, mx, my), daddy::variant(variant));
                    reports::add_log(std::string{daddy::to_string(daddy::variant(variant))} + " long legs");
                    break;
                }
                case SDLK_U: {
                    if (e.key.mod & SDL_KMOD_CTRL) {
                        npc::destroy_all(gs);
                        reports::add_log("NPC slugcats removed");
                        break;
                    }
                    // a group of three, maybe a pack
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    npc::spawn_group(gs, a.room, render::screen_to_room(r, a.cam, mx, my), 10, gs.random);
                    reports::add_log("three NPC slugcats; " + npc::stats_line(gs));
                    break;
                }
                case SDLK_I: {
                    // spawns the next kind of item at the cursor
                    static uint8_t next = uint8_t(world::item_type::explosive_spear);
                    float mx = 0.f, my = 0.f;
                    SDL_GetMouseState(&mx, &my);
                    vec2 const at = render::screen_to_room(r, a.cam, mx, my);
                    items::create(gs, a.room, at, world::item_type(next));
                    if (++next >= uint8_t(world::item_type::count))
                        next = uint8_t(world::item_type::rock);
                    break;
                }
                case SDLK_P: paused = !paused; break;
                case SDLK_M:
                    if (sound && (e.key.mod & SDL_KMOD_CTRL))
                        audio::stop_song(*sound);
                    else if (sound)
                        audio::force_song(*sound, gs, w.room_get_region_from_region_membership(a.room));
                    break;
                case SDLK_N: step_once = true; break;
                case SDLK_F6: gs.storm = !gs.storm; break;
                case SDLK_F7:
                    // the rain of the region (and the regions linked to it) in 30 seconds
                    rain::rain_in(gs, w.room_get_rain_region(a.room), 30 * 40);
                    break;
                case SDLK_LEFTBRACKET: case SDLK_RIGHTBRACKET: {
                    // the Watcher's ripple level in half steps, any other slugcat's karma
                    int32_t const d = e.key.key == SDLK_RIGHTBRACKET ? 1 : -1;
                    if (warps::is_watcher(gs, a.players[0])) {
                        gs.ripple_level = std::clamp(gs.ripple_level + 0.5f * float(d), 0.f, 5.f);
                    } else {
                        gs.karma = std::clamp(gs.karma + d, 0, 9);
                        gs.karma_cap = std::max(gs.karma_cap, gs.karma);
                    }
                    break;
                }
                case SDLK_T: slow = !slow; break;
                case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4: case SDLK_5:
                case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9: case SDLK_0: {
                    uint32_t const k = e.key.key == SDLK_0 ? 9u : uint32_t(e.key.key - SDLK_1);
                    // 0 again while playing the Watcher: its Void Weaver look
                    if (k == 9 && a.slugcat == player::slugcat_name::watcher) {
                        bool const on = !player_graphics::weaver(gs, a.players[0]);
                        player_graphics::set_weaver(gs, a.players[0], on);
                        reports::add_log(on ? "watcher: decorated" : "watcher: original look");
                        break;
                    }
                    a.slugcat = player::slugcat_name(k);
                    // rebuild player one where it stands (body mass depends on the slugcat)
                    auto const old = a.players[0];
                    bool const travelling = player::in_shortcut(gs, old);
                    dcon::room_id const pr = travelling ? a.room : simulation::player_room(gs, old);
                    vec2 const feet = travelling ? a.spawn : w.body_chunk_get_pos(player::body_chunk(gs, old, 1));
                    player::destroy(gs, old);
                    a.players[0] = player::create(gs, pr, feet, a.slugcat);
                    player_graphics::reset(gs, a.players[0]);
                    reports::add_log(std::string{"slugcat: "} + player::to_string(a.slugcat));
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
        std::string hud = a.hud(paused, slow);
        if (sound) {
            vec2 const size{float(r.width) / a.cam.zoom, float(r.height) / a.cam.zoom};
            vec2 const center = a.cam.pos + size * 0.5f;
            audio::update(*sound, gs, a.room, center, size, a.listener(center), float(frame) / 1e9f);
            auto const song = audio::current_song(*sound);
            if (!song.empty())
                hud += "\nmusic: " + song;
        } else {
            gs.sounds.clear();
            gs.music.clear();
        }
        render::draw_frame(r, gs, a.view(), alpha, a.cam, hud);
        render::present(r);
    }

    if (pad != nullptr)
        SDL_CloseGamepad(pad);
    sound.reset();
    render::shutdown(r);
    return 0;
}
