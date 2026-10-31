#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "spawns.hpp"
#include "needle_worm.hpp"
#include "dens.hpp"
#include "centipede.hpp"
#include "daddy.hpp"
#include "leech.hpp"
#include "lizard.hpp"
#include "reports.hpp"
#include "text.hpp"
#include "world.hpp"

namespace spawns {
    namespace {
        std::string_view trim(std::string_view v) {
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r'))
                v.remove_prefix(1);
            while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
                v.remove_suffix(1);
            return v;
        }
        std::vector<std::string_view> split(std::string_view sv, std::string_view sep) {
            std::vector<std::string_view> r;
            for (size_t p = 0;;) {
                size_t const q = sv.find(sep, p);
                r.push_back(sv.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p));
                if (q == std::string_view::npos)
                    return r;
                p = q + sep.size();
            }
        }
        std::string lower_compact(std::string_view s) {
            std::string r;
            for (char const c : s)
                if (c != ' ' && c != '_')
                    r += char(std::tolower(static_cast<unsigned char>(c)));
            return r;
        }

        // SlugcatStats.Timeline's name for a campaign
        char const* timeline(::player::slugcat_name n) noexcept {
            static char const* const names[] = {"White", "Yellow", "Red", "Gourmand", "Artificer", "Rivulet", "Spear", "Saint", "Inv", "Watcher", "Slugpup"};
            return n < ::player::slugcat_name::count ? names[size_t(n)] : "";
        }

        // WorldLoader.Preprocessing.TimelineMatch
        bool timeline_match(std::string_view text, ::player::slugcat_name story) {
            bool exclude = false;
            if (text.starts_with("X-")) {
                text.remove_prefix(2);
                exclude = true;
            }
            bool match = false;
            for (auto part : split(text, ",")) {
                part = trim(part);
                if (!part.empty() && std::isdigit(static_cast<unsigned char>(part[0]))) {
                    // the old player numbers: 0 Survivor, 1 Monk, 2 Hunter
                    int const n = std::atoi(std::string{part}.c_str());
                    match = match || (n >= 0 && n <= 2 && int(story) == n);
                } else {
                    match = match || part == timeline(story);
                }
            }
            return match != exclude;
        }

        // one "Type-{data}-count" / "Type-{data}-chance" entry: the numbers
        // after the type, the data between braces (which may hold dashes)
        void split_entry(std::vector<std::string_view> const& parts, size_t from, std::string& data, std::string& number) {
            bool in_data = false;
            for (size_t j = from; j < parts.size(); ++j) {
                auto const p = parts[j];
                if (!p.empty() && p[0] == '{') {
                    data = std::string{p};
                    in_data = true;
                } else if (in_data) {
                    data += "-" + std::string{p};
                } else {
                    number = std::string{p};
                }
                if (p.find('}') != std::string_view::npos)
                    in_data = false;
            }
        }

        // the CREATURES lines of every root's modify/world/<region>/world_<region>.txt [MERGE] blocks
        std::string merged_creature_lines(sys::game_state const& state, std::string const& region) {
            std::string out;
            for (auto it = state.fs.roots.rbegin(); it != state.fs.roots.rend(); ++it) {
                std::ifstream in(it->root + "/modify/world/" + region + "/world_" + region + ".txt");
                if (!in)
                    continue;
                std::stringstream ss;
                ss << in.rdbuf();
                std::string const text = ss.str();
                bool merge = false, creatures = false;
                for (auto line : split(text, "\n")) {
                    line = trim(line);
                    if (line == "[MERGE]") {
                        merge = true;
                        continue;
                    }
                    if (line == "[ENDMERGE]") {
                        merge = false;
                        creatures = false;
                        continue;
                    }
                    if (!merge)
                        continue;
                    if (line == "CREATURES") {
                        creatures = true;
                        continue;
                    }
                    if (line == "END CREATURES") {
                        creatures = false;
                        continue;
                    }
                    if (creatures)
                        out += std::string{line} + "\n";
                }
            }
            return out;
        }
    }

    std::vector<spawn> parse(std::string_view text, ::player::slugcat_name story) {
        std::vector<spawn> out;
        bool in = false;
        // a world file's whole text, or bare creature lines (the merges)
        bool const bare = text.find("CREATURES") == std::string_view::npos;
        for (auto line : split(text, "\n")) {
            line = trim(line);
            if (line == "CREATURES") {
                in = true;
                continue;
            }
            if (line == "END CREATURES") {
                in = false;
                continue;
            }
            if (!(in || bare))
                continue;
            // WorldLoader.Preprocessing.PreprocessLine
            if (line.size() < 2 || line.starts_with("//"))
                continue;
            if (line[0] == '(' && line.find(')') != std::string_view::npos) {
                size_t const close = line.find(')');
                if (!timeline_match(line.substr(1, close - 1), story))
                    continue;
                line = line.substr(close + 1);
            }
            // (custom conditions {...}: none of them hold for a fresh save
            // that the port knows of, the line is kept like vanilla keeps
            // the ones it doesn't recognise)
            if (!line.empty() && line[0] == '{' && line.find('}') != std::string_view::npos)
                line = line.substr(line.find('}') + 1);
            auto const fields = split(line, " : ");
            if (fields.size() < 2)
                continue;
            if (trim(fields[0]) == "LINEAGE") {
                // LINEAGE : ROOM : den : Type-chance, ... (: night)
                if (fields.size() < 4)
                    continue;
                // (Regex.Split on ", ": a comma inside the spawn data braces stays)
                auto const steps = split(fields[3], ", ");
                auto const first = split(trim(steps[0]), "-");
                spawn s;
                s.lineage = true;
                s.room = std::string{trim(fields[1])};
                s.den = std::atoi(std::string{trim(fields[2])}.c_str());
                s.type = std::string{trim(first[0])};
                std::string number;
                split_entry(first, 1, s.data, number);
                s.night = fields.size() > 4;
                out.push_back(std::move(s));
                continue;
            }
            // ROOM : den-Type-{data}-count, ...
            for (auto entry : split(fields[1], ", ")) {
                entry = trim(entry);
                if (entry.empty())
                    continue;
                auto const parts = split(entry, "-");
                if (parts.size() < 2)
                    continue;
                spawn s;
                s.room = std::string{trim(fields[0])};
                s.den = std::atoi(std::string{parts[0]}.c_str());
                s.type = std::string{trim(parts[1])};
                std::string number;
                split_entry(parts, 2, s.data, number);
                if (!number.empty()) {
                    char* end = nullptr;
                    long const n = std::strtol(number.c_str(), &end, 10);
                    s.count = end != number.c_str() ? int32_t(n) : 1;
                }
                out.push_back(std::move(s));
            }
        }
        return out;
    }

    std::optional<creature::spawn_desc> kind_of(std::string_view name) noexcept {
        using creature::type;
        std::string const t = lower_compact(name);
        auto const make = [](type k, uint8_t v, float size = -1.f) { return creature::spawn_desc{k, size, false, v}; };
        struct breed_name {
            char const* name;
            lizard::breed b;
        };
        static constexpr breed_name breeds[] = {
            {"pink", lizard::breed::pink}, {"green", lizard::breed::green}, {"blue", lizard::breed::blue},
            {"yellow", lizard::breed::yellow}, {"white", lizard::breed::white}, {"red", lizard::breed::red},
            {"black", lizard::breed::black}, {"salamander", lizard::breed::salamander}, {"cyan", lizard::breed::cyan},
            {"spit", lizard::breed::caramel}, {"caramel", lizard::breed::caramel}, {"eel", lizard::breed::eel},
            {"zoop", lizard::breed::zoop}, {"train", lizard::breed::train}, {"blizzard", lizard::breed::blizzard},
            {"basilisk", lizard::breed::basilisk}, {"indigo", lizard::breed::indigo}, {"peach", lizard::breed::peach},
        };
        for (auto const& b : breeds) {
            std::string const n = b.name;
            if (t == n || t == n + "lizard")
                return make(type::lizard, uint8_t(b.b));
        }
        if (t == "centipede" || t == "cent")
            return make(type::centipede, uint8_t(centipede::variant::centipede));
        if (t == "smallcentipede")
            return make(type::centipede, uint8_t(centipede::variant::small));
        if (t == "centiwing")
            return make(type::centipede, uint8_t(centipede::variant::wing));
        if (t == "redcentipede" || t == "redcenti")
            return make(type::centipede, uint8_t(centipede::variant::red));
        if (t == "aquacenti" || t == "aquapede")
            return make(type::centipede, uint8_t(centipede::variant::aqua));
        if (t == "leech")
            return make(type::leech, uint8_t(leech::variant::leech));
        if (t == "sealeech")
            return make(type::leech, uint8_t(leech::variant::sea));
        if (t == "jungleleech")
            return make(type::leech, uint8_t(leech::variant::jungle));
        if (t == "dropbug" || t == "dropwig")
            return make(type::dropbug, 0);
        if (t == "tubeworm" || t == "tube" || t == "grappleworm")
            return make(type::tube_worm, 0);
        if (t == "bigneedleworm" || t == "bigneedle" || t == "needleworm" || t == "needle" || t == "noodlefly")
            return make(type::needle_worm, uint8_t(needle_worm::variant::big));
        if (t == "smallneedleworm" || t == "smallneedle" || t == "babynoodlefly")
            return make(type::needle_worm, uint8_t(needle_worm::variant::small));
        if (t == "polemimic" || t == "mimic" || t == "poleplant")
            return make(type::pole_mimic, 0);
        if (t == "tentacleplant" || t == "tentacle" || t == "monsterkelp")
            return make(type::tentacle_plant, 0);
        if (t == "jetfish")
            return make(type::jetfish, 0);
        if (t == "scavenger" || t == "scav")
            return make(type::scavenger, 0);
        if (t == "deer" || t == "raindeer")
            return make(type::deer, 0);
        if (t == "mirosbird" || t == "miros")
            return make(type::miros, 0);
        // a cicada's size: 1 the white CicadaA
        if (t == "cicadaa")
            return make(type::cicada, 0, 1.f);
        if (t == "cicadab")
            return make(type::cicada, 0, 0.f);
        // either, at random (spawn_in_den picks by the creature's number)
        if (t == "cicada")
            return make(type::cicada, 0, -2.f);
        if (t == "daddylonglegs" || t == "daddy")
            return make(type::daddy, uint8_t(daddy::variant::daddy));
        if (t == "brolonglegs" || t == "bro" || t == "brotherlonglegs")
            return make(type::daddy, uint8_t(daddy::variant::brother));
        if (t == "terrorlonglegs" || t == "terror" || t == "motherlonglegs" || t == "mother")
            return make(type::daddy, uint8_t(daddy::variant::mother));
        if (t == "hunterdaddy")
            return make(type::daddy, uint8_t(daddy::variant::hunter));
        return std::nullopt;
    }

    spawn_data parse_data(std::string_view data) noexcept {
        spawn_data r;
        if (data.size() < 2 || data.front() != '{')
            return r;
        data = data.substr(1, data.size() - 2);
        size_t p = 0;
        while (p <= data.size()) {
            size_t q = data.find_first_of(",|", p);
            if (q == std::string_view::npos)
                q = data.size();
            auto const flag = trim(data.substr(p, q - p));
            p = q + 1;
            if (flag.empty())
                continue;
            auto const key = flag.substr(0, flag.find(':'));
            auto const value = flag.find(':') == std::string_view::npos ? std::string_view{} : flag.substr(flag.find(':') + 1);
            if (key == "Night") {
                r.flags |= dens::flag_night;
                r.flags &= uint8_t(~dens::flag_ignore_cycle);
            } else if (key == "Ignorecycle") {
                r.flags |= dens::flag_ignore_cycle;
            } else if (key == "PreCycle") {
                r.flags |= dens::flag_pre_cycle;
            } else if (key == "TentacleImmune") {
                r.flags |= dens::flag_tentacle_immune;
            } else if (key == "Lavasafe") {
                r.flags |= dens::flag_lava_safe;
            } else if (key == "Winter") {
                r.flags |= dens::flag_winter;
            } else if (key == "AlternateForm") {
                r.flags |= dens::flag_alternate_form;
            } else if (key == "Voidsea") {
                r.flags |= dens::flag_void_sea | dens::flag_lava_safe;
            } else if (key == "Seed") {
                r.seed = std::atoi(std::string{value}.c_str());
            } else if (key == "Mean") {
                // Lizard's constructor reads it from the unrecognised flags
                r.mean = std::strtof(std::string{value}.c_str(), nullptr);
            } else {
                // an unrecognised flag: a centipede's size when it is a number
                std::string const s{flag};
                char* end = nullptr;
                float const f = std::strtof(s.c_str(), &end);
                if (end != s.c_str() && *end == '\0')
                    r.size = f;
            }
        }
        return r;
    }

    uint32_t spawn_in_den(sys::game_state& state, dcon::room_id room, spawn const& s, report& r) {
        auto& w = state.world;
        auto const k = kind_of(s.type);
        if (!k) {
            r.skipped[s.type] += uint32_t(std::max(1, s.count));
            return 0;
        }
        auto const miss = [&](uint32_t& n) {
            ++n;
            if (r.missing.size() < 8)
                r.missing.push_back(s.room + ":" + std::to_string(s.den) + " " + s.type);
            return 0u;
        };
        if (!room)
            return miss(r.no_room);
        auto const entrance = dens::den_entrance(state, room, uint8_t(s.den));
        if (!entrance)
            return miss(r.no_den);
        auto const data = parse_data(s.data);
        uint8_t flags = data.flags;
        if (s.night)
            flags = uint8_t((flags | dens::flag_night) & ~dens::flag_ignore_cycle);
        creature::spawn_desc d = *k;
        if (d.kind == creature::type::centipede && d.variety == uint8_t(centipede::variant::centipede) && data.size >= 0.f)
            d.size = data.size;
        // a Daddy in its AlternateForm is the Mother's size
        if (d.kind == creature::type::daddy && d.variety == uint8_t(daddy::variant::daddy) && (flags & dens::flag_alternate_form))
            d.variety = uint8_t(daddy::variant::mother);
        uint32_t made = 0;
        int32_t const n = std::max(1, s.count);
        for (int32_t i = 0; i < n; ++i) {
            // RainWorldGame.GetNewID: the next number, the seed unless {Seed:N}
            int32_t const number = ++state.next_issued_id;
            int32_t const seed = data.seed >= 0 ? data.seed : number;
            if (d.kind == creature::type::cicada && k->size < -1.f)
                d.size = number % 2 != 0 ? 1.f : 0.f;
            auto const saved = state.random;
            state.random = rw::rng{uint32_t(seed) * 0x9E3779B1u ^ 0x5BD1E995u};
            if (state.random.state == 0)
                state.random.state = 0x2545F491u;
            state.spawn_seed = seed;
            auto const c = creature::create(state, room, world::room_view::middle_of_tile(*entrance), d);
            state.spawn_seed = -1;
            state.random = saved;
            if (!c)
                continue;
            w.creature_set_spawn_flags(c, flags);
            if (d.kind == creature::type::lizard)
                w.creature_get_payload(c).lizard.spawn_data_evil = data.mean;
            dens::set_home(state, c, room, uint8_t(s.den));
            // AbstractCreature's constructor: out of the den in 100 to 1000 ticks
            dens::put_in_den(state, c, w.creature_get_random(c).range(100, 1000));
            // (the plants' remainInDenCounter is 0: out at once, a pole
            // plant to stand as a pole, a monster kelp to root)
            if (d.kind == creature::type::pole_mimic || d.kind == creature::type::tentacle_plant)
                w.creature_set_remain_in_den(c, 0);
            ++made;
            std::string key = creature::to_string(d.kind);
            switch (d.kind) {
            case creature::type::lizard: key = std::string{lizard::to_string(lizard::breed(d.variety))} + " lizard"; break;
            case creature::type::centipede: key = centipede::to_string(centipede::variant(d.variety)); break;
            case creature::type::leech: key = leech::to_string(leech::variant(d.variety)); break;
            case creature::type::daddy: key = std::string{daddy::to_string(daddy::variant(d.variety))} + " long legs"; break;
            case creature::type::cicada: key = d.size != 0.f ? "white squidcada (CicadaA)" : "dark squidcada (CicadaB)"; break;
            case creature::type::needle_worm: key = d.variety == uint8_t(needle_worm::variant::small) ? "baby noodlefly" : "noodlefly"; break;
            default: break;
            }
            ++r.made[key];
            ++r.creatures;
            r.night += (flags & dens::flag_night) ? 1 : 0;
            r.pre_cycle += (flags & dens::flag_pre_cycle) ? 1 : 0;
            r.ignore_cycle += (flags & dens::flag_ignore_cycle) ? 1 : 0;
        }
        return made;
    }

    report spawn_world(sys::game_state& state) {
        auto& w = state.world;
        report r;
        auto const world_dir = state.fs.get_root().get_single_subdir("world");
        w.for_each_region([&](dcon::region_id region) {
            if (w.region_get_is_pseudo(region))
                return;
            std::string const name{state.to_string_view(w.region_get_name(region))};
            std::string text;
            if (auto const of = world_dir.get_single_subdir(name).get_single_file("world_" + name + ".txt").open_file(state.fs); of)
                text = std::string{of->to_string_view()};
            auto list = parse(text, state.story_slugcat);
            auto const merged = merged_creature_lines(state, name);
            if (!merged.empty()) {
                auto more = parse(merged, state.story_slugcat);
                list.insert(list.end(), more.begin(), more.end());
            }
            for (auto const& s : list) {
                if (s.lineage) {
                    if (text::to_lowercase_native(s.type) == "none") {
                        ++r.lineage_none;
                        continue;
                    }
                    if (kind_of(s.type))
                        ++r.lineages;
                }
                if (s.room == "OFFSCREEN") {
                    // the offscreen den: only kinds that aren't ported live there
                    if (kind_of(s.type))
                        ++r.no_room;
                    else
                        r.skipped[s.type] += uint32_t(std::max(1, s.count));
                    continue;
                }
                spawn_in_den(state, state.find_room(text::to_lowercase_native(s.room)), s, r);
            }
        });
        return r;
    }

    std::string to_string(report const& r) {
        std::string s = "world file creatures: " + std::to_string(r.creatures) + " made in their dens (" + std::to_string(r.lineages)
            + " from lineages, " + std::to_string(r.lineage_none) + " lineages starting with NONE; " + std::to_string(r.night) + " night, "
            + std::to_string(r.pre_cycle) + " precycle (they stay in), " + std::to_string(r.ignore_cycle) + " ignoring the cycle; "
            + std::to_string(r.no_room) + " without their room, " + std::to_string(r.no_den) + " without their den)\n  made:";
        for (auto const& [k, n] : r.made)
            s += " " + k + " " + std::to_string(n) + ",";
        uint32_t skipped = 0;
        for (auto const& [k, n] : r.skipped)
            skipped += n;
        if (!r.missing.empty()) {
            s += "\n  missing:";
            for (auto const& m : r.missing)
                s += " " + m + ",";
        }
        s += "\n  not ported (" + std::to_string(skipped) + "):";
        for (auto const& [k, n] : r.skipped)
            s += " " + k + " " + std::to_string(n) + ",";
        return s;
    }
}
