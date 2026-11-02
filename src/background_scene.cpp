#include "background_scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "stb_image.h"

#include "gpu.hpp"
#include "layered_fs.hpp"
#include "math.hpp"
#include "reports.hpp"
#include "text.hpp"

namespace render::background_scene {
    namespace {
        using rw::vec2;
        using rw::vec3;
        namespace mathf = rw::mathf;

        // background.frag's modes
        enum class mode : int32_t {
            background = 0,         // Background: the texture times the colour
            additive = 1,           // BackgroundAdditive
            color = 2,              // Background on the "pixel" element: a plain colour
            cloud = 3,              // Cloud
            cloud_distant = 4,      // CloudDistant
            distant_bkg_object = 5, // DistantBkgObject
        };
        // background.frag's flags
        constexpr int32_t flag_procedural_main = 1;   // the cloud illustration is missing
        constexpr int32_t flag_procedural_clouds = 2; // cloudsTexture is missing

        // background.frag's block (std140)
        struct bkg_uniforms {
            glm::mat4 proj{1.f};
            glm::vec4 clr{1.f};        // the sprite's vertex colour
            glm::vec4 atmosphere{1.f}; // _AboveCloudsAtmosphereColor
            glm::vec4 multiply{1.f};   // _MultiplyColor
            glm::vec2 cam_pos{0.f};    // vanilla's camPos (the view's bottom left), room pixels
            glm::vec2 vscreen{1366.f, 768.f}; // vanilla's screen size
            float rain = 0.f;          // _RAIN
            float wind_dir = -1.f;     // _windDir (-1 with MSC)
            float night_time = 0.f;
            int32_t mode = 0;
            int32_t index = 0;         // palette layer
            int32_t flags = 0;
            float pad0 = 0.f, pad1 = 0.f;
        };
        static_assert(offsetof(bkg_uniforms, clr) == 64 && offsetof(bkg_uniforms, cam_pos) == 112
            && offsetof(bkg_uniforms, rain) == 128 && offsetof(bkg_uniforms, flags) == 148 && sizeof(bkg_uniforms) == 160);

        struct tex_entry {
            gpu::texture tex = 0; // 0: missing
            int w = 0, h = 0;
        };

        // DistantLightning's flicker (Update and LightIntensity), render-side
        struct lightning_state {
            rw::unity_random rng{0};
            int32_t wait = 0;
            int32_t tiny_thunder_wait = 5;
            int32_t tiny_thunder = 0;
            int32_t tiny_thunder_length = 0;
            int32_t thunder = 0;
            int32_t thunder_length = 0;
            float random_level = 0.f;
            float power = 0.f;
            int32_t random_level_change = 0;
            float last_intensity = 0.f;
            float intensity = 0.f;
        };

        enum class element_type : uint8_t {
            sky,         // Simple2DBackgroundIllustration (the day sky)
            building,    // DistantBuilding
            lightning,   // DistantLightning
            horizon_fog, // HorizonFog
            additive,    // AdditiveBackgroundIllustration (the pink sun)
            close_cloud, // CloseCloud
            distant_cloud,
            flying_cloud,
            fog,         // AboveCloudsView.Fog
        };

        struct element {
            element_type type = element_type::sky;
            std::string asset;
            vec2 pos{0.f, 0.f};
            float depth = 0.f;
            float sort_depth = 0.f; // the depth BackgroundScene sorted them by
            float atmos_add = 0.f;  // DistantBuilding.atmosphericalDepthAdd
            float scale = 1.f;
            float cloud_depth = 0.f; // CloseCloud.cloudDepth, DistantCloud.distantCloudDepth
            int32_t index = 0;
            float random_offset = 0.f;
            float flattened = 0.f, alpha = 1.f, shader_input = 0.f; // FlyingCloud
            lightning_state ls;
        };

        enum class scene_kind : uint8_t { none, above_clouds, pink_sky };

        struct room_scene {
            scene_kind kind = scene_kind::none;
            bool si_clouds = false;
            bool oe_clouds = false;
            float start_altitude = 20000.f;
            float end_altitude = 31400.f;
            float clouds_start_depth = 5.f;
            float clouds_end_depth = 40.f;
            float distant_clouds_end_depth = 200.f;
            vec2 origo{0.f, 0.f};       // sceneOrigo
            vec2 map_pos{0.f, 0.f};     // AbstractRoom.mapPos
            vec2 size{0.f, 0.f};        // the room, tiles
            vec3 atmosphere{0.16078432f, 0.23137255f, 0.31764707f};
            std::vector<element> elements; // in drawing order
            uint32_t last_tick = 0;
            bool ticked = false;
        };

        struct cache {
            gpu::context* gpu = nullptr;
            std::unordered_map<std::string, tex_entry> textures;
            std::unordered_map<uint32_t, room_scene> rooms;
        };
        cache& the_cache() {
            static cache c;
            return c;
        }

        // ---- the room's settings (RoomSettings: the room's effects, then
        // its template's that it doesn't set itself)

        std::string lower(std::string_view sv) {
            return text::to_lowercase_native(sv);
        }

        // the rest of the line starting with `key` (at a line's start)
        std::string_view line_value(std::string_view text, std::string_view key) {
            size_t at = text.find(key);
            while (at != std::string_view::npos && at != 0 && text[at - 1] != '\n')
                at = text.find(key, at + 1);
            if (at == std::string_view::npos)
                return {};
            std::string_view line = text.substr(at + key.size());
            line = line.substr(0, line.find('\n'));
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.remove_suffix(1);
            return line;
        }

        // the effect types of an "Effects:" line, in order
        void effect_names(std::string_view text, std::vector<std::string>& out) {
            std::string_view const line = line_value(text, "Effects: ");
            for (size_t p = 0; p < line.size();) {
                size_t const q = line.find(',', p);
                std::string_view e = line.substr(p, q == std::string_view::npos ? std::string_view::npos : q - p);
                p = q == std::string_view::npos ? line.size() : q + 1;
                while (!e.empty() && (e.front() == ' ' || e.front() == '\r'))
                    e.remove_prefix(1);
                size_t const dash = e.find('-');
                // (the extra amounts of ModifyEffectColor* come after commas: no dash, no name)
                if (dash == std::string_view::npos || dash == 0)
                    continue;
                std::string name{e.substr(0, dash)};
                if (std::find(out.begin(), out.end(), name) == out.end())
                    out.push_back(std::move(name));
            }
        }

        std::string read_file(sys::game_state const& s, layered_fs::file const& f) {
            auto const of = f.open_file(s.fs);
            return of ? std::string{of->to_string_view()} : std::string{};
        }

        float parse_float(std::string_view sv) {
            return std::strtof(std::string{sv}.c_str(), nullptr);
        }

        // ---- textures (BackgroundScene.LoadGraphic: Illustrations/<name>.png)

        tex_entry const& load_illustration(state& r, sys::game_state const& s, std::string const& name, gpu::sampler samp) {
            auto& c = the_cache();
            std::string const key = lower(name) + (samp == gpu::sampler::linear_repeat ? "#l" : "#n");
            if (auto const it = c.textures.find(key); it != c.textures.end())
                return it->second;
            tex_entry e;
            auto const f = s.fs.get_root().get_single_subdir("illustrations").get_single_file(lower(name) + ".png");
            if (auto const of = f.open_file(s.fs); of) {
                auto const sv = of->to_string_view();
                int w = 0, h = 0, n = 0;
                stbi_uc* px = stbi_load_from_memory(reinterpret_cast<stbi_uc const*>(sv.data()), int(sv.size()), &w, &h, &n, 4);
                if (px != nullptr) {
                    e.tex = gpu::create_texture(*r.gpu, w, h, 0, samp);
                    if (e.tex != 0) {
                        gpu::upload(*r.gpu, e.tex, 0, px, 4);
                        e.w = w;
                        e.h = h;
                    }
                    stbi_image_free(px);
                } else {
                    reports::add_log("can't decode \"" + f.path + "\"");
                }
            } else {
                reports::add_log("background scene: missing illustration \"" + name + "\"");
            }
            return c.textures.emplace(key, e).first->second;
        }

        // ---- building the scene (AboveCloudsView's constructor)

        void add_building(room_scene& sc, std::string asset, vec2 input, float depth, float atmos_add, float scale = 1.f) {
            element e;
            e.type = element_type::building;
            e.asset = std::move(asset);
            // PosFromDrawPosAtNeutralCamPos, run before BackgroundScene.Update
            // sets convergencePoint (still zero): input * depth
            e.pos = input * depth;
            e.depth = e.sort_depth = depth;
            e.atmos_add = atmos_add;
            e.scale = scale;
            sc.elements.push_back(std::move(e));
        }
        void add_lightning(room_scene& sc, std::string asset, vec2 input, float depth, float minus_depth, uint32_t seed) {
            element e;
            e.type = element_type::lightning;
            e.asset = std::move(asset);
            e.pos = input * depth;
            e.depth = depth;
            // sorted at depth - minusDepthForLayering, drawn at depth (its first Update restores it)
            e.sort_depth = depth - minus_depth;
            e.ls.rng = rw::unity_random{int32_t(seed)};
            sc.elements.push_back(std::move(e));
        }

        void build_scene(room_scene& sc, sys::game_state const& s, std::string const& room_name, uint32_t room_index) {
            bool const pink = sc.kind == scene_kind::pink_sky;
            rw::unity_random rnd{12}; // Random.InitState(12)
            uint32_t lightning_seed = room_index * 977u + 1u;
            auto const light_seed = [&] { return lightning_seed++; };

            // the day sky (the dusk and night skies under it only show
            // through the DayNight fade, not ported)
            {
                element e;
                e.type = element_type::sky;
                e.asset = pink ? "pnk_sky" : "AtC_Sky";
                e.pos = vec2{683.f, 384.f};
                e.depth = e.sort_depth = std::numeric_limits<float>::max();
                sc.elements.push_back(std::move(e));
            }
            {
                element e;
                e.type = element_type::fog;
                e.depth = e.sort_depth = 0.f;
                e.alpha = 1.f;
                sc.elements.push_back(std::move(e));
            }
            if (sc.si_clouds)
                add_building(sc, "AtC_Spire1", vec2{517.f, -148.f}, 50.f, -30.f);
            if (pink) {
                sc.atmosphere = vec3{0.682f, 0.286f, 0.529f};
                add_building(sc, "pnk_structure2", vec2{-674.f, -50.9427f}, 196.1146f, 0.2f, 0.1233124f);
                add_building(sc, "pnk_structure1", vec2{-651.f, -135.1355f}, 79.7291f, 0.2f, 0.8216254f);
                add_building(sc, "pnk_structure2", vec2{-580.f, -80.f}, 205.f, 0.f, 0.4f);
                add_building(sc, "pnk_structure3", vec2{-442.f, -134.1671f}, 81.66579f, 0.2f, 0.8100052f);
                add_building(sc, "pnk_structure1", vec2{-308.f, -124.9811f}, 130.0379f, 0.2f, 0.6997727f);
                add_building(sc, "pnk_structure2", vec2{-100.f, -145.9757f}, 58.04863f, 0.2f, 0.9517083f);
                add_building(sc, "pnk_structure1", vec2{-60.f, -96.92464f}, 156.1507f, 0.2f, 0.3630957f);
                add_building(sc, "pnk_structure1", vec2{513.f, -124.9381f}, 100.1239f, 0.2f, 0.6992567f);
                add_building(sc, "pnk_structure1", vec2{577.f, -108.278f}, 133.444f, 0.2f, 0.4993357f);
                add_building(sc, "pnk_structure3", vec2{700.f, -250.f}, 70.f, 0.2f, 1.f);
                element fog;
                fog.type = element_type::horizon_fog;
                fog.asset = "pnk_fog";
                fog.pos = vec2{683.f, 384.f};
                fog.depth = fog.sort_depth = sc.distant_clouds_end_depth - 5.f;
                sc.elements.push_back(std::move(fog));
                element sun;
                sun.type = element_type::additive;
                sun.asset = "pnk_sun";
                sun.pos = vec2{683.f, 384.f};
                sun.depth = sun.sort_depth = mathf::lerp(sc.clouds_start_depth, sc.clouds_end_depth, 0.5f);
                sc.elements.push_back(std::move(sun));
            } else if (sc.oe_clouds) {
                // (OE_TREETOP, OE_JUNGLE02 and OE_SPIRE's local spire, retaining
                // wall and Five Pebbles aren't ported)
            } else if (!sc.si_clouds || room_name == "si_a07" || room_name == "si_saintintro") {
                float const ox = sc.si_clouds ? 200.f : 0.f;
                bool const saint = s.story_slugcat == ::player::slugcat_name::saint;
                std::string const str = saint ? "SAINT" : "";
                auto const b = [&](char const* name, float x, float y, float depth, float add) {
                    add_building(sc, std::string{name} + str, vec2{x + ox, y}, depth, add);
                };
                auto const l = [&](char const* name, float x, float y, float depth, float minus) {
                    if (!saint)
                        add_lightning(sc, name, vec2{x + ox, y}, depth, minus, light_seed());
                };
                b("AtC_Structure1", -520.f, -85.f, 160.f, -20.f);
                l("AtC_Light1", -520.f, -119.f, 160.f, 55.f);
                b("AtC_Structure2", 88.f, -37.f, 350.f, 0.f);
                l("AtC_Light2", 88.f, -53.f, 350.f, 250.f);
                b("AtC_Structure3", -316.f, -24.f, 600.f, -100.f);
                l("AtC_Light3", -316.f, -32.f, 600.f, 80.f);
                b("AtC_Structure4", -648.f, -21.f, 700.f, -200.f);
                b("AtC_Structure5", 156.f, -22.f, 800.f, -350.f);
                b("AtC_Structure6", -242.f, -20.f, 850.f, -350.f);
                b("AtC_Spire1", -587.f, -134.f, 80.f, -60.f);
                b("AtC_Spire2", -653.f, -57.f, 100.f, 10.f);
                b("AtC_Spire3", 0.f, -46.f, 155.f, 0.f);
                b("AtC_Spire4", -224.f, -20.f, 190.f, 80.f);
                b("AtC_Spire5", -276.f, -24.f, 360.f, -100.f);
                b("AtC_Spire6", -39.f, -33.f, 280.f, 0.f);
                b("AtC_Spire7", 155.f, -7.f, 370.f, -85.f);
                b("AtC_Spire8", -380.f, 3.f, 380.f, -50.f);
                b("AtC_Spire9", -207.f, -1.f, 395.f, -50.f);
                b("AtC_Structure1", 670.f, -85.f, 120.f, -20.f);
                l("AtC_Light1", 670.f, -119.f, 120.f, 55.f);
                b("AtC_Spire2", 470.f, -57.f, 160.f, 10.f);
                b("AtC_Spire5", 220.f, -24.f, 365.f, -100.f);
                b("AtC_Spire6", 520.f, -33.f, 285.f, 0.f);
                b("AtC_Spire7", 340.f, -7.f, 375.f, -85.f);
                b("AtC_Spire8", 600.f, 3.f, 380.f, -50.f);
                b("AtC_Spire9", 175.f, -1.f, 390.f, -50.f);
            } else {
                // the other SI rooms
                auto const b = [&](char const* name, float x, float y, float depth, float add) {
                    add_building(sc, name, vec2{x, y}, depth, add);
                };
                auto const l = [&](char const* name, float x, float y, float depth, float minus) {
                    add_lightning(sc, name, vec2{x, y}, depth, minus, light_seed());
                };
                b("AtC_Structure1", -520.f, -85.f, 160.f, -20.f);
                l("AtC_Light1", -520.f, -119.f, 160.f, 55.f);
                b("AtC_Structure2", 88.f, -37.f, 350.f, 0.f);
                l("AtC_Light2", 88.f, -53.f, 350.f, 250.f);
                b("AtC_Structure3", -316.f, -24.f, 600.f, -100.f);
                l("AtC_Light3", -316.f, -32.f, 600.f, 80.f);
                b("AtC_Structure4", -648.f, -21.f, 700.f, -200.f);
                b("AtC_Structure5", 156.f, -22.f, 800.f, -350.f);
                b("AtC_Structure6", -242.f, -20.f, 850.f, -350.f);
                b("AtC_Spire1", -587.f, -134.f, 80.f, -60.f);
                b("AtC_Spire2", -653.f, -57.f, 100.f, 10.f);
                b("AtC_Spire3", 0.f, -46.f, 155.f, 0.f);
                b("AtC_Spire4", -224.f, -20.f, 190.f, 80.f);
                b("AtC_Spire5", -276.f, -24.f, 360.f, -100.f);
                b("AtC_Spire6", -39.f, -33.f, 280.f, 0.f);
                b("AtC_Spire7", 155.f, -7.f, 370.f, -85.f);
                b("AtC_Spire8", -380.f, 3.f, 380.f, -50.f);
                b("AtC_Spire9", -207.f, -1.f, 395.f, -50.f);
            }
            // the close clouds (AboveCloudsView and PinkSky both)
            for (int32_t i = 0; i < 7; ++i) {
                element e;
                e.type = element_type::close_cloud;
                e.cloud_depth = float(i) / 6.f;
                e.depth = e.sort_depth = mathf::lerp(sc.clouds_start_depth, sc.clouds_end_depth, e.cloud_depth);
                e.index = i;
                e.asset = "clouds" + std::to_string(i % 3 + 1);
                e.random_offset = rnd.value();
                sc.elements.push_back(std::move(e));
            }
            if (!sc.si_clouds || room_name == "si_a07" || room_name == "si_saintintro") {
                for (int32_t j = 0; j < 11; ++j) {
                    element e;
                    e.type = element_type::distant_cloud;
                    float const f = float(j) / 10.f;
                    e.cloud_depth = f;
                    e.pos = vec2{0.f, -40.f * sc.clouds_end_depth * (1.f - f)};
                    e.depth = e.sort_depth = mathf::lerp(sc.clouds_end_depth, sc.distant_clouds_end_depth, std::pow(f, 1.5f));
                    e.index = j;
                    e.asset = "clouds" + std::to_string(j % 3 + 1);
                    e.random_offset = rnd.value();
                    sc.elements.push_back(std::move(e));
                }
                auto const flying = [&](float y, float depth, float flattened, float alpha, float shader_input) {
                    element e;
                    e.type = element_type::flying_cloud;
                    e.asset = "flyingClouds1";
                    e.pos = vec2{0.f, y} * depth;
                    e.depth = e.sort_depth = depth;
                    e.random_offset = rnd.value();
                    e.flattened = flattened;
                    e.alpha = alpha;
                    e.shader_input = shader_input;
                    sc.elements.push_back(std::move(e));
                };
                flying(75.f, 355.f, 0.35f, 0.5f, 0.9f);
                flying(43.f, 920.f, 0.15f, 0.3f, 0.95f);
            }
            // BackgroundScene.Update: added to the room by depth, farthest
            // first (OrderByDescending keeps ties in insertion order)
            std::stable_sort(sc.elements.begin(), sc.elements.end(), [](element const& a, element const& b) { return a.sort_depth > b.sort_depth; });
        }

        room_scene resolve(sys::game_state const& s, world::room_view const& room) {
            room_scene sc;
            auto const& w = s.world;
            std::string const name = lower(s.to_string_view(w.room_get_name(room.id)));
            auto const region_id = w.room_get_region_from_region_membership(room.id);
            if (!region_id)
                return sc;
            std::string const region = lower(s.to_string_view(w.region_get_name(region_id)));
            bool const pseudo = w.region_get_is_pseudo(region_id);
            auto const world_dir = s.fs.get_root().get_single_subdir("world");
            auto const room_dir = pseudo ? world_dir.get_single_subdir(region) : world_dir.get_single_subdir(region + "-rooms");
            auto const region_dir = world_dir.get_single_subdir(region);
            std::string const props = pseudo ? std::string{} : read_file(s, region_dir.get_single_file("properties.txt"));

            // the effects: the room's, then its template's (RoomSettings.InheritEffects)
            std::string const settings = read_file(s, room_dir.get_single_file(name + "_settings.txt"));
            std::vector<std::string> effects;
            effect_names(settings, effects);
            std::string template_piece;
            std::string const tline = lower(line_value(settings, "Template: "));
            if (tline.empty()) {
                // the region's first template is every room's default
                std::string_view list = line_value(props, "Room Setting Templates: ");
                list = list.substr(0, list.find(','));
                while (!list.empty() && list.back() == ' ')
                    list.remove_suffix(1);
                template_piece = lower(list);
            } else if (tline != "none") {
                // "SU_SettingsTemplate_Inside": the third '_' piece
                std::string_view piece{tline};
                for (int i = 0; i < 2 && piece.find('_') != std::string_view::npos; ++i)
                    piece.remove_prefix(piece.find('_') + 1);
                template_piece = std::string{piece.substr(0, piece.find('_'))};
            }
            if (!template_piece.empty() && !pseudo)
                effect_names(read_file(s, region_dir.get_single_file(region + "_settingstemplate_" + template_piece + ".txt")), effects);
            // Room.Loaded: an AboveCloudsView for either effect, whatever its amount
            for (auto const& e : effects) {
                if (e == "AboveCloudsView") {
                    sc.kind = scene_kind::above_clouds;
                    break;
                }
                if (e == "PinkSky") {
                    sc.kind = scene_kind::pink_sky;
                    break;
                }
            }
            if (sc.kind == scene_kind::none)
                return sc;

            // the altitudes (MSC on: LM / SL, OE; then the region's cloudsStart / cloudsEnd)
            sc.si_clouds = region == "si" || name.starts_with("si_");
            sc.oe_clouds = region == "oe" || name.starts_with("oe_");
            if (sc.si_clouds) {
                sc.start_altitude = 9000.f;
                sc.end_altitude = 26400.f;
            }
            if (region == "lm" || region == "sl") {
                sc.start_altitude = 21000.f;
                sc.end_altitude = 32400.f;
            }
            if (sc.oe_clouds) {
                sc.start_altitude = 0.f;
                sc.end_altitude = 36900.f;
            }
            if (auto const v = line_value(props, "cloudsStart: "); !v.empty())
                sc.start_altitude = parse_float(v);
            if (auto const v = line_value(props, "cloudsEnd: "); !v.empty())
                sc.end_altitude = parse_float(v);
            sc.origo = vec2{2514.f, (sc.start_altitude + sc.end_altitude) / 2.f};

            // the room's place on the region map (World.LoadMapConfig: "<ROOM>: x><y><...")
            if (!pseudo) {
                std::string const map = read_file(s, region_dir.get_single_file("map_" + region + ".txt"));
                std::string_view rest{map};
                while (!rest.empty()) {
                    size_t const nl = rest.find('\n');
                    std::string_view line = rest.substr(0, nl);
                    rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
                    size_t const colon = line.find(": ");
                    if (colon == std::string_view::npos || lower(line.substr(0, colon)) != name)
                        continue;
                    std::string_view v = line.substr(colon + 2);
                    size_t const sep = v.find("><");
                    if (sep == std::string_view::npos)
                        break;
                    sc.map_pos = vec2{parse_float(v.substr(0, sep)), parse_float(v.substr(sep + 2))};
                    break;
                }
            }
            if (region == "lc")
                sc.map_pos = vec2{500.f, 5000.f};
            sc.size = vec2{float(room.width), float(room.height)};
            build_scene(sc, s, name, uint32_t(room.id.index()));
            return sc;
        }

        // ---- drawing

        // BackgroundScene.RoomToWorldPos
        vec2 room_to_world(room_scene const& sc, vec2 in_room) {
            return (sc.map_pos / 3.f + vec2{10.f, 10.f}) * 20.f + in_room - sc.size * 20.f / 2.f;
        }

        struct view_frame {
            vec2 cam_pos;     // camPos (and the camera position the clouds read)
            vec2 conv;        // convergencePoint
            float h_displace; // RoomCamera.hDisplace
            vec2 view;        // the view's size, room pixels (vanilla's screen coordinates span it)
        };

        // BackgroundScene.DrawPos
        vec2 draw_pos(room_scene const& sc, view_frame const& v, vec2 pos, float depth, vec2 cam_pos) {
            vec2 a = pos + sc.origo - room_to_world(sc, cam_pos);
            a.x += v.h_displace - 8.f;
            return (a - v.conv) / depth + v.conv;
        }

        void lightning_reset(lightning_state& l) {
            l.wait = int32_t(mathf::lerp(10.f, 440.f, l.rng.value()) * mathf::lerp(1.5f, 1.f, 1.f));
            l.power = mathf::lerp(0.7f, 1.f, l.rng.value());
            l.thunder_length = l.rng.range(1, int32_t(mathf::lerp(10.f, 32.f, l.power)));
        }
        // DistantLightning.Update
        void lightning_update(lightning_state& l) {
            if (--l.random_level_change < 1) {
                l.random_level_change = l.rng.range(1, 6);
                l.random_level = l.rng.value();
            }
            if (l.wait > 0) {
                if (--l.wait < 1)
                    l.thunder = l.thunder_length;
            } else if (--l.thunder < 1) {
                lightning_reset(l);
            }
            if (l.tiny_thunder_wait > 0 && --l.tiny_thunder_wait < 1) {
                l.tiny_thunder_wait = l.rng.range(10, 80);
                l.tiny_thunder_length = l.rng.range(5, l.tiny_thunder_wait);
                l.tiny_thunder = l.tiny_thunder_length;
            }
            l.last_intensity = l.intensity;
            float a = 0.f, b = 0.f;
            if (l.thunder > 0 && l.thunder_length > 0) {
                float const fac = 1.f - float(l.thunder) / float(l.thunder_length);
                a = std::pow(l.random_level, mathf::lerp(3.f, 0.1f, std::sin(fac * mathf::pi)));
            }
            if (l.tiny_thunder > 0) {
                --l.tiny_thunder;
                float const fac = l.tiny_thunder_length > 0 ? 1.f - float(l.tiny_thunder) / float(l.tiny_thunder_length) : 0.f;
                b = std::pow(l.rng.value(), mathf::lerp(3.f, 0.1f, std::sin(fac * mathf::pi))) * 0.4f;
            }
            l.intensity = mathf::max(a, b);
        }
        // DistantLightning.LightIntensity (no timeStacker: the latest)
        float lightning_intensity(lightning_state& l) {
            float num = l.intensity;
            if (l.rng.value() < 0.33333334f)
                num = mathf::lerp(num, l.rng.value() < 0.5f ? 1.f : 0.f, l.rng.value() * num);
            return rw::custom::s_curve(num, 0.5f);
        }

        struct draw_cmd {
            bkg_uniforms u;
            gpu::texture tex = 0;
            size_t first = 0;
        };

        struct builder {
            std::vector<gpu::vertex> verts;
            std::vector<draw_cmd> cmds;
            bkg_uniforms base;
            vec2 cam_pos{0.f, 0.f};

            // a quad in vanilla's screen coordinates (room pixels from camPos),
            // uv Unity's (v from the bottom)
            void quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, mode m, glm::vec4 clr, gpu::texture tex, int32_t flags = 0) {
                draw_cmd c;
                c.u = base;
                c.u.mode = int32_t(m);
                c.u.clr = clr;
                c.u.flags = flags;
                c.tex = tex;
                c.first = verts.size();
                float const ax = cam_pos.x + x0, ay = cam_pos.y + y0, bx = cam_pos.x + x1, by = cam_pos.y + y1;
                gpu::vertex const bl{ax, ay, 0xFFFFFFFFu, u0, v0};
                gpu::vertex const br{bx, ay, 0xFFFFFFFFu, u1, v0};
                gpu::vertex const tr{bx, by, 0xFFFFFFFFu, u1, v1};
                gpu::vertex const tl{ax, by, 0xFFFFFFFFu, u0, v1};
                verts.insert(verts.end(), {bl, br, tr, bl, tr, tl});
                cmds.push_back(c);
            }
        };

        // a texture-repeating sprite of `w` x `h` centred on x, its top at
        // `top`, widened (its uv along) to span the view [x_lo, x_hi]
        void wide_sprite(builder& b, float x, float top, float w, float h, float x_lo, float x_hi, mode m, glm::vec4 clr, gpu::texture tex, int32_t flags) {
            float const left = x - w * 0.5f;
            float const x0 = std::min(left, x_lo), x1 = std::max(left + w, x_hi);
            b.quad(x0, top - h, x1, top, (x0 - left) / w, 0.f, (x1 - left) / w, 1.f, m, clr, tex, flags);
        }

        // an illustration centred on `c`, scaled up (about `c`) if it doesn't cover the view
        void covering_illustration(builder& b, vec2 c, float w, float h, float scale_y, vec2 view, mode m, glm::vec4 clr, gpu::texture tex) {
            float const hw = w * 0.5f, hh = h * scale_y * 0.5f;
            float const kx = std::max({1.f, c.x / hw, (view.x - c.x) / hw});
            // (the horizon fog, a band, isn't stretched to the view's height)
            float const ky = scale_y == 1.f ? std::max({1.f, c.y / hh, (view.y - c.y) / hh}) : 1.f;
            float const k = std::max(kx, ky);
            b.quad(c.x - hw * k, c.y - hh * k, c.x + hw * k, c.y + hh * k, 0.f, 0.f, 1.f, 1.f, m, clr, tex);
        }
    }

    bool draw(state& r, sys::game_state const& s, world::room_view const& room, frame_params const& f) {
        if (r.gpu == nullptr || !room.id)
            return false;
        auto& c = the_cache();
        if (c.gpu != r.gpu) {
            c = cache{};
            c.gpu = r.gpu;
        }
        uint32_t const key = uint32_t(room.id.index());
        auto it = c.rooms.find(key);
        if (it == c.rooms.end())
            it = c.rooms.emplace(key, resolve(s, room)).first;
        room_scene& sc = it->second;
        if (sc.kind == scene_kind::none || sc.elements.empty())
            return false;

        // the lightning's Update, once per tick shown (a long gap restarts it)
        if (!sc.ticked || s.tick < sc.last_tick || s.tick - sc.last_tick > 400u) {
            sc.ticked = true;
            sc.last_tick = s.tick;
        }
        uint32_t const steps = std::min<uint32_t>(s.tick - sc.last_tick, 40u);
        sc.last_tick = s.tick;
        for (auto& e : sc.elements)
            if (e.type == element_type::lightning)
                for (uint32_t i = 0; i < steps; ++i)
                    lightning_update(e.ls);

        view_frame v;
        vec2 const screen{s.screen_width, s.screen_height};
        v.cam_pos = f.cam.pos;
        v.conv = vec2{screen.x / 2.f, screen.y * 2.f / 3.f};
        v.h_displace = (1400.f - screen.x) / 2.f - 8.f;
        v.view = vec2{f.view_w, f.view_h};
        float const x_lo = -20.f, x_hi = v.view.x + 20.f;
        float const y_shift = 0.f; // (OE's yShift isn't ported)

        // Cloud.ApplyPalette / Fog.ApplyPalette: the palette's sky colour (its pixel (0, 7))
        vec3 sky{0.f, 0.f, 0.f};
        size_t const pal_bytes = size_t(palette_width) * palette_height * 3;
        if (f.palette >= 0 && r.palette_pixels.size() >= (size_t(f.palette) + 1) * pal_bytes) {
            uint8_t const* px = r.palette_pixels.data() + size_t(f.palette) * pal_bytes;
            uint8_t const* rain_px = px + size_t(8) * palette_width * 3;
            for (int i = 0; i < 3; ++i)
                sky[i] = mathf::lerp(float(px[i]) / 255.f, float(rain_px[i]) / 255.f, f.night_time);
        }
        vec3 const atmos = sc.atmosphere;
        auto const sky_atmos = [&](float t) {
            vec3 const c3 = sky + (atmos - sky) * mathf::clamp01(t);
            return glm::vec4{c3.x, c3.y, c3.z, 1.f};
        };

        builder b;
        b.cam_pos = v.cam_pos;
        b.base.proj = f.proj;
        b.base.atmosphere = glm::vec4{atmos.x, atmos.y, atmos.z, 1.f};
        b.base.multiply = glm::vec4{1.f};
        b.base.cam_pos = v.cam_pos;
        b.base.vscreen = screen;
        b.base.rain = f.rain;
        b.base.wind_dir = -1.f;
        b.base.night_time = f.night_time;
        b.base.index = f.palette;

        auto const& clouds_tex = load_illustration(r, s, "cloudsTexture", gpu::sampler::linear_repeat);
        int32_t const cloud_flags = clouds_tex.tex == 0 ? flag_procedural_clouds : 0;
        // the camera's altitude (the clouds read the camera position, the fog camPos: both the view's here)
        float const cam_world_y = room_to_world(sc, v.cam_pos).y;
        float const altitude = mathf::inverse_lerp(sc.start_altitude, sc.end_altitude, cam_world_y);
        float const altitude_shifted = mathf::inverse_lerp(sc.start_altitude, sc.end_altitude, cam_world_y + y_shift);
        vec2 const cam_shifted{v.cam_pos.x, v.cam_pos.y + y_shift};

        for (auto& e : sc.elements) {
            switch (e.type) {
            case element_type::sky: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::nearest_clamp);
                if (t.tex == 0)
                    break; // (the frame's clear colour is the sky colour)
                covering_illustration(b, e.pos, float(t.w), float(t.h), 1.f, v.view, mode::background, glm::vec4{1.f, 1.f, 1.f, e.alpha}, t.tex);
                break;
            }
            case element_type::building: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::nearest_clamp);
                if (t.tex == 0)
                    break;
                vec2 const p = draw_pos(sc, v, e.pos, e.depth, cam_shifted);
                float const w = float(t.w) * e.scale, h = float(t.h) * e.scale;
                float const shade = std::pow(mathf::inverse_lerp(0.f, 600.f, e.depth + e.atmos_add), 0.3f) * 0.9f;
                // anchorY 0
                b.quad(p.x - w * 0.5f, p.y, p.x + w * 0.5f, p.y + h, 0.f, 0.f, 1.f, 1.f, mode::distant_bkg_object, glm::vec4{shade, 0.f, 0.f, 1.f}, t.tex);
                break;
            }
            case element_type::lightning: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::nearest_clamp);
                if (t.tex == 0)
                    break;
                vec2 const p = draw_pos(sc, v, e.pos, e.depth, cam_shifted);
                float const a = lightning_intensity(e.ls) * 0.2f * mathf::inverse_lerp(27600.f, 31400.f, cam_world_y + y_shift);
                if (a <= 0.f)
                    break;
                float const w = float(t.w), h = float(t.h);
                b.quad(p.x - w * 0.5f, p.y, p.x + w * 0.5f, p.y + h, 0.f, 0.f, 1.f, 1.f, mode::background, glm::vec4{1.f, 1.f, 1.f, a}, t.tex);
                break;
            }
            case element_type::horizon_fog: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::nearest_clamp);
                if (t.tex == 0)
                    break;
                float const y = draw_pos(sc, v, e.pos, e.depth, vec2{v.cam_pos.x, v.cam_pos.y + y_shift + 30000.f}).y;
                covering_illustration(b, vec2{e.pos.x, y}, float(t.w), float(t.h), 1.5f, v.view, mode::background, glm::vec4{1.f}, t.tex);
                break;
            }
            case element_type::additive: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::nearest_clamp);
                if (t.tex == 0)
                    break;
                covering_illustration(b, e.pos, float(t.w), float(t.h), 1.f, v.view, mode::additive, glm::vec4{1.f, 1.f, 1.f, e.alpha}, t.tex);
                break;
            }
            case element_type::close_cloud: {
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::linear_repeat);
                float const tw = t.tex != 0 ? float(t.w) : 700.f, th = t.tex != 0 ? float(t.h) : 150.f;
                float num2 = e.cloud_depth;
                if (altitude > 0.5f)
                    num2 = mathf::lerp(num2, 1.f, mathf::inverse_lerp(0.5f, 1.f, altitude) * 0.5f);
                e.depth = mathf::lerp(sc.clouds_start_depth, sc.clouds_end_depth, num2);
                float const num3 = mathf::lerp(10.f, 2.f, num2);
                float num4 = draw_pos(sc, v, e.pos, e.depth, cam_shifted).y;
                num4 += mathf::lerp(std::pow(e.cloud_depth, 0.75f), std::sin(e.cloud_depth * mathf::pi), 0.5f) * mathf::inverse_lerp(0.5f, 0.f, altitude) * 600.f;
                num4 -= mathf::inverse_lerp(0.18f, 0.1f, altitude) * std::pow(1.f - e.cloud_depth, 3.f) * 100.f;
                float num5 = mathf::lerp(1.f, mathf::lerp(0.75f, 0.25f, altitude), num2);
                if (sc.si_clouds) {
                    num4 += -100.f + 200.f * num2;
                    num5 = 1.f;
                }
                float const h = th * num5 * num3;
                float const top = num4 - 2.f;
                // the fill under it (1400 x 500 from the cloud's bottom, down to the view's bottom here)
                float const fill_top = num4 - h;
                b.quad(std::min(683.f - 700.f, x_lo), std::min(fill_top - 500.f, -20.f), std::max(683.f + 700.f, x_hi), fill_top, 0.f, 0.f, 1.f, 1.f,
                    mode::color, sky_atmos(num2 * 0.75f), 0);
                wide_sprite(b, 683.f, top, tw * num3, h, x_lo, x_hi, mode::cloud,
                    glm::vec4{num2 * 0.75f, e.random_offset, mathf::lerp(num5, 1.f, 0.5f), 1.f}, t.tex,
                    cloud_flags | (t.tex == 0 ? flag_procedural_main : 0));
                break;
            }
            case element_type::distant_cloud: {
                if (altitude_shifted < 0.33f)
                    break;
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::linear_repeat);
                float const tw = t.tex != 0 ? float(t.w) : 700.f, th = t.tex != 0 ? float(t.h) : 150.f;
                constexpr float num = 2.f;
                float const y = draw_pos(sc, v, e.pos, e.depth, cam_shifted).y;
                float num2 = mathf::lerp(0.3f, 0.01f, e.cloud_depth);
                if (e.index == 8)
                    num2 *= 1.5f;
                float const mix_t = mathf::lerp(0.75f, 0.95f, e.cloud_depth);
                // the fill from the screen's bottom up to the cloud
                float const fill_top = y - th * num * num2;
                if (fill_top > -20.f)
                    b.quad(std::min(683.f - 700.f, x_lo), -20.f, std::max(683.f + 700.f, x_hi), fill_top, 0.f, 0.f, 1.f, 1.f, mode::color, sky_atmos(mix_t), 0);
                wide_sprite(b, 683.f, y - 2.f, tw * num, th * num2 * num, x_lo, x_hi, mode::cloud_distant,
                    glm::vec4{mix_t, e.random_offset, mathf::lerp(num2, 1.f, 0.5f), 1.f}, t.tex,
                    cloud_flags | (t.tex == 0 ? flag_procedural_main : 0));
                break;
            }
            case element_type::flying_cloud: {
                if (altitude < 0.33f)
                    break;
                auto const& t = load_illustration(r, s, e.asset, gpu::sampler::linear_repeat);
                float const tw = t.tex != 0 ? float(t.w) : 700.f, th = t.tex != 0 ? float(t.h) : 150.f;
                constexpr float num = 2.f;
                float const y = draw_pos(sc, v, e.pos, e.depth, v.cam_pos).y;
                wide_sprite(b, 683.f, y, tw * num, th * e.flattened * num, x_lo, x_hi, mode::cloud_distant,
                    glm::vec4{e.shader_input, e.random_offset, mathf::lerp(e.flattened, 1.f, 0.5f), e.alpha}, t.tex,
                    cloud_flags | (t.tex == 0 ? flag_procedural_main : 0));
                break;
            }
            case element_type::fog: {
                float alpha;
                float const value = cam_world_y + y_shift;
                if (sc.oe_clouds) {
                    vec2 const wp = room_to_world(sc, v.cam_pos);
                    alpha = mathf::inverse_lerp(0.f, 66900.f, wp.x) * 0.5f;
                    alpha = mathf::lerp(alpha, 0.6f, mathf::inverse_lerp(21207.4f, 19887.3f, wp.y));
                } else if (sc.si_clouds) {
                    alpha = mathf::inverse_lerp(16000.f, 9000.f, value) * 0.6f;
                } else {
                    alpha = mathf::inverse_lerp(22000.f, 18000.f, value) * 0.6f;
                }
                if (sc.kind == scene_kind::pink_sky)
                    alpha = mathf::inverse_lerp(9000.f, 5700.f, value) * 0.6f;
                if (alpha <= 0.f)
                    break;
                glm::vec4 col{sky.x, sky.y, sky.z, alpha};
                b.quad(-20.f, -20.f, v.view.x + 20.f, v.view.y + 20.f, 0.f, 0.f, 1.f, 1.f, mode::color, col, 0);
                break;
            }
            }
        }
        if (b.cmds.empty())
            return true;
        gpu::upload_vertices(*r.gpu, b.verts.data(), b.verts.size());
        for (auto const& cmd : b.cmds)
            gpu::draw(*r.gpu, gpu::pipeline::background, &cmd.u, sizeof(cmd.u), {cmd.tex, r.palette_texarray, clouds_tex.tex}, cmd.first, 6);
        return true;
    }
}
