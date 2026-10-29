#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "render.hpp"
#include "centipede.hpp"
#include "gates.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player_graphics.hpp"
#include "rot.hpp"
#include "warps.hpp"
#include "particles.hpp"
#include "water.hpp"
#include "terrain.hpp"
#include "shortcuts.hpp"
#include "reports.hpp"

namespace render {
    using rw::vec2;
    using world::terrain;
    namespace mathf = rw::mathf;

    namespace {
        constexpr uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
            return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24);
        }
        constexpr uint32_t col_background = rgba(18, 18, 24);
        constexpr uint32_t col_solid = rgba(92, 92, 104);
        constexpr uint32_t col_floor = rgba(150, 130, 90);
        constexpr uint32_t col_wall_behind = rgba(34, 34, 44);
        constexpr uint32_t col_beam = rgba(170, 120, 60);
        constexpr uint32_t col_shortcut_entrance = rgba(230, 200, 40);
        constexpr uint32_t col_shortcut_path = rgba(120, 100, 30);
        constexpr uint32_t col_water = rgba(40, 80, 170, 90);
        constexpr uint32_t col_camera = rgba(80, 160, 80, 90);
        constexpr uint32_t col_bounds = rgba(200, 60, 60, 160);
        constexpr uint32_t col_body = rgba(235, 235, 235);
        constexpr uint32_t col_body_dead = rgba(150, 60, 60);
        constexpr uint32_t col_eye = rgba(20, 20, 30);
        constexpr uint32_t col_rock = rgba(14, 12, 16);
        constexpr uint32_t col_needle = rgba(235, 228, 240);
        constexpr uint32_t col_gate_door = rgba(22, 20, 26);
        constexpr uint32_t col_gate_door_edge = rgba(48, 44, 52);

        char const* const vertex_src = R"(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_uv;
uniform mat4 u_proj;
out vec4 v_color;
out vec2 v_uv;
void main() {
    v_color = a_color;
    v_uv = a_uv;
    gl_Position = u_proj * vec4(a_pos, 0.0, 1.0);
}
)";
        char const* const fragment_src = R"(#version 330 core
in vec4 v_color;
in vec2 v_uv;
uniform sampler2D u_texture;
uniform int u_textured;
out vec4 frag_color;
void main() {
    frag_color = u_textured != 0 ? v_color * texture(u_texture, v_uv) : v_color;
}
)";

        GLuint compile(GLenum type, char const* src) {
            GLuint const sh = glCreateShader(type);
            glShaderSource(sh, 1, &src, nullptr);
            glCompileShader(sh);
            GLint ok = 0;
            glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
                reports::add_log(std::string{"shader: "} + log);
            }
            return sh;
        }

        // Batch helpers, all in room pixel coordinates
        struct painter {
            std::vector<vertex>& b;
            void tri(vec2 a, vec2 c, vec2 d, uint32_t col) {
                b.push_back(vertex{a.x, a.y, col, 0.f, 0.f});
                b.push_back(vertex{c.x, c.y, col, 0.f, 0.f});
                b.push_back(vertex{d.x, d.y, col, 0.f, 0.f});
            }
            void rect(float x, float y, float w, float h, uint32_t col) {
                tri(vec2{x, y}, vec2{x + w, y}, vec2{x + w, y + h}, col);
                tri(vec2{x, y}, vec2{x + w, y + h}, vec2{x, y + h}, col);
            }
            void line(vec2 a, vec2 c, float width, uint32_t col) {
                vec2 const n = rw::custom::perpendicular_vector(c - a) * (width * 0.5f);
                tri(a - n, c - n, c + n, col);
                tri(a - n, c + n, a + n, col);
            }
            void outline(float x, float y, float w, float h, float width, uint32_t col) {
                line(vec2{x, y}, vec2{x + w, y}, width, col);
                line(vec2{x + w, y}, vec2{x + w, y + h}, width, col);
                line(vec2{x + w, y + h}, vec2{x, y + h}, width, col);
                line(vec2{x, y + h}, vec2{x, y}, width, col);
            }
            // textured quad: (x, y) top left in screen pixels (y down), uv rect in [0, 1]
            void glyph(float x, float y, float w, float h, float u0, float v0, float u1, float v1, uint32_t col) {
                vertex const tl{x, y, col, u0, v0}, tr{x + w, y, col, u1, v0}, br{x + w, y + h, col, u1, v1}, bl{x, y + h, col, u0, v1};
                b.insert(b.end(), {tl, tr, br, tl, br, bl});
            }
            void circle(vec2 c, float r, uint32_t col) {
                constexpr int segs = 16;
                for (int i = 0; i < segs; ++i) {
                    float const a0 = float(i) / float(segs) * 6.2831853f;
                    float const a1 = float(i + 1) / float(segs) * 6.2831853f;
                    tri(c, c + vec2{std::cos(a0) * r, std::sin(a0) * r}, c + vec2{std::cos(a1) * r, std::sin(a1) * r}, col);
                }
            }
            // radial gradient from `inner` at the centre to `outer` at the rim
            void glow(vec2 c, float r, uint32_t inner, uint32_t outer) {
                constexpr int segs = 32;
                for (int i = 0; i < segs; ++i) {
                    float const a0 = float(i) / float(segs) * 6.2831853f;
                    float const a1 = float(i + 1) / float(segs) * 6.2831853f;
                    b.push_back(vertex{c.x, c.y, inner, 0.f, 0.f});
                    b.push_back(vertex{c.x + std::cos(a0) * r, c.y + std::sin(a0) * r, outer, 0.f, 0.f});
                    b.push_back(vertex{c.x + std::cos(a1) * r, c.y + std::sin(a1) * r, outer, 0.f, 0.f});
                }
            }
        };
    }

    bool init(state& r, int width, int height, char const* title, bool hidden) {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
            reports::add_log(std::string{"SDL_Init: "} + SDL_GetError());
            return false;
        }
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
        if (hidden)
            flags |= SDL_WINDOW_HIDDEN;
        r.window = SDL_CreateWindow(title, width, height, flags);
        if (r.window == nullptr) {
            reports::add_log(std::string{"SDL_CreateWindow: "} + SDL_GetError());
            return false;
        }
        r.context = SDL_GL_CreateContext(r.window);
        if (r.context == nullptr) {
            reports::add_log(std::string{"SDL_GL_CreateContext: "} + SDL_GetError());
            return false;
        }
        SDL_GL_MakeCurrent(r.window, r.context);
        SDL_GL_SetSwapInterval(1);

        glewExperimental = GL_TRUE;
        GLenum const err = glewInit();
        // a GLX build of GLEW reports "no GLX display" under EGL (Wayland,
        // offscreen) even though every GL entry point was loaded
        if (err != GLEW_OK && err != GLEW_ERROR_NO_GLX_DISPLAY) {
            reports::add_log(std::string{"glewInit: "} + reinterpret_cast<char const*>(glewGetErrorString(err)));
            return false;
        }
        if (glCreateShader == nullptr) {
            reports::add_log("OpenGL 3.3 entry points missing");
            return false;
        }

        GLuint const vs = compile(GL_VERTEX_SHADER, vertex_src);
        GLuint const fs = compile(GL_FRAGMENT_SHADER, fragment_src);
        r.program = glCreateProgram();
        glAttachShader(r.program, vs);
        glAttachShader(r.program, fs);
        glLinkProgram(r.program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        r.u_proj = glGetUniformLocation(r.program, "u_proj");
        r.u_textured = glGetUniformLocation(r.program, "u_textured");
        glUseProgram(r.program);
        glUniform1i(glGetUniformLocation(r.program, "u_texture"), 0);

        glGenVertexArrays(1, &r.vao);
        glGenBuffers(1, &r.vbo);
        glBindVertexArray(r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, r.vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), reinterpret_cast<void*>(offsetof(vertex, x)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(vertex), reinterpret_cast<void*>(offsetof(vertex, rgba)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), reinterpret_cast<void*>(offsetof(vertex, u)));

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        r.width = width;
        r.height = height;
        return true;
    }

    namespace {
        struct image {
            stbi_uc* px = nullptr;
            int w = 0, h = 0;
            image() = default;
            image(image const&) = delete;
            ~image() { if (px != nullptr) stbi_image_free(px); }
        };
        bool load_image(layered_fs::filesystem const& fs, layered_fs::file const& f, int channels, image& out) {
            auto const of = f.open_file(fs);
            if (!of)
                return false;
            auto const sv = of->to_string_view();
            int n = 0;
            out.px = stbi_load_from_memory(reinterpret_cast<stbi_uc const*>(sv.data()), int(sv.size()), &out.w, &out.h, &n, channels);
            if (out.px == nullptr)
                reports::add_log("can't decode \"" + f.path + "\": " + stbi_failure_reason());
            return out.px != nullptr;
        }
        GLuint make_texture_2d(image const& im, GLenum format, GLenum internal_format) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal_format), im.w, im.h, 0, format, GL_UNSIGNED_BYTE, im.px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return tex;
        }
        std::string read_text(layered_fs::filesystem const& fs, layered_fs::file const& f) {
            auto const of = f.open_file(fs);
            return of ? std::string{of->to_string_view()} : std::string{};
        }
        GLuint link(GLuint vs, GLuint fs) {
            GLuint const prog = glCreateProgram();
            glAttachShader(prog, vs);
            glAttachShader(prog, fs);
            glLinkProgram(prog);
            GLint ok = 0;
            glGetProgramiv(prog, GL_LINK_STATUS, &ok);
            if (!ok) {
                char log[512];
                glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
                reports::add_log(std::string{"link: "} + log);
            }
            glDeleteShader(vs);
            glDeleteShader(fs);
            return prog;
        }

        // anthiel's VGA font: 9x16 cells, 32 per row, starting at '!'
        void load_font(state& r, layered_fs::filesystem const& fs) {
            image im;
            if (!load_image(fs, fs.get_root().get_single_file("vgafont.png"), 4, im)) {
                reports::add_log("can't find vgafont.png, the HUD is disabled");
                return;
            }
            r.font_texture = make_texture_2d(im, GL_RGBA, GL_RGBA8);
            r.font_width = im.w;
            r.font_height = im.h;
        }

        // room shader (assets/shaders/room.vert/frag, from anthiel)
        void load_room_shader(state& r, layered_fs::filesystem const& fs) {
            auto const dir = fs.get_root().get_single_subdir("shaders");
            auto const vs_src = read_text(fs, dir.get_single_file("room.vert"));
            auto const fs_src = read_text(fs, dir.get_single_file("room.frag"));
            if (vs_src.empty() || fs_src.empty()) {
                reports::add_log("missing shaders/room.vert or room.frag, rooms are drawn as geometry only");
                return;
            }
            std::string const prelude = "#version 330 core\n#define MAX_LIGHTS " + std::to_string(max_lights) + "\n";
            r.room_program = link(compile(GL_VERTEX_SHADER, (prelude + vs_src).c_str()), compile(GL_FRAGMENT_SHADER, (prelude + fs_src).c_str()));
            auto& u = r.room_uniforms;
            auto const loc = [&](char const* n) { return glGetUniformLocation(r.room_program, n); };
            u.proj = loc("u_proj");
            u.palette = loc("u_index");
            u.camera_index = loc("u_camera_index");
            u.night_time = loc("u_night_time");
            u.fog_factor = loc("u_fog_factor");
            u.effect_colors = loc("u_effect_colors");
            u.grime = loc("u_grime_factor");
            u.rooftop_blend = loc("u_rooftop_blend");
            u.timer = loc("u_timer");
            u.lighting_data = loc("u_lighting_data");
            u.lighting_pos = loc("u_lighting_pos");
            u.replace_a = loc("u_replace_effect_color_a");
            u.replace_b = loc("u_replace_effect_color_b");
            u.pass = loc("u_pass");
            glUseProgram(r.room_program);
            glUniform1i(loc("u_texture0"), 0);
            glUniform1i(loc("u_texture1"), 1);
            glUniform1i(loc("u_texture2"), 2);
        }

        // the warp shaders (assets/shaders/warp.vert, warp_common.glsl, warp_tear.frag, warp_screen.frag)
        void load_warp_shaders(state& r, layered_fs::filesystem const& fs) {
            auto const dir = fs.get_root().get_single_subdir("shaders");
            auto const vs_src = read_text(fs, dir.get_single_file("warp.vert"));
            auto const common = read_text(fs, dir.get_single_file("warp_common.glsl"));
            auto const tear = read_text(fs, dir.get_single_file("warp_tear.frag"));
            auto const screen = read_text(fs, dir.get_single_file("warp_screen.frag"));
            if (vs_src.empty() || common.empty() || tear.empty() || screen.empty()) {
                reports::add_log("missing warp shaders, warps are drawn without their effects");
                return;
            }
            std::string const prelude = "#version 330 core\n";
            auto const make = [&](std::string const& frag) {
                GLuint const prog = link(compile(GL_VERTEX_SHADER, (prelude + vs_src).c_str()), compile(GL_FRAGMENT_SHADER, (prelude + common + frag).c_str()));
                glUseProgram(prog);
                glUniform1i(glGetUniformLocation(prog, "u_scene"), 0);
                return prog;
            };
            r.warp_tear_program = make(tear);
            r.warp_screen_program = make(screen);
            // the water body reuses the warp vertex shader (a_uv: room x, depth)
            auto const water_src = read_text(fs, dir.get_single_file("water.frag"));
            if (!water_src.empty())
                r.water_program = make(water_src);
            auto const dvs = read_text(fs, dir.get_single_file("decal.vert"));
            auto const dfs = read_text(fs, dir.get_single_file("decal.frag"));
            if (!dvs.empty() && !dfs.empty()) {
                r.decal_program = link(compile(GL_VERTEX_SHADER, (prelude + dvs).c_str()), compile(GL_FRAGMENT_SHADER, (prelude + dfs).c_str()));
                glUseProgram(r.decal_program);
                glUniform1i(glGetUniformLocation(r.decal_program, "u_level"), 0);
                glUniform1i(glGetUniformLocation(r.decal_program, "u_palette"), 1);
                glUniform1i(glGetUniformLocation(r.decal_program, "u_decal"), 2);
            } else {
                reports::add_log("missing shaders/decal.vert or decal.frag, decals are not drawn");
            }
        }

        // every palette object becomes one layer of a texture array (like
        // anthiel), plus the global effectcolors.png table
        void load_palettes(state& r, sys::game_state const& s) {
            auto const& w = s.world;
            auto const dir = s.fs.get_root().get_single_subdir("palettes");
            uint32_t const count = std::max<uint32_t>(1, w.palette_size());
            r.palette_pixels.assign(size_t(count) * palette_width * palette_height * 3, 0);
            glGenTextures(1, &r.palette_texarray);
            glBindTexture(GL_TEXTURE_2D_ARRAY, r.palette_texarray);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB8, palette_width, palette_height, GLsizei(count), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
            w.for_each_palette([&](dcon::palette_id pid) {
                auto const number = w.palette_get_number(pid);
                image im;
                if (!load_image(s.fs, dir.get_single_file("palette" + std::to_string(number) + ".png"), 3, im)) {
                    reports::add_log("missing palette #" + std::to_string(number));
                    return;
                }
                if (im.w != palette_width || im.h != palette_height) {
                    reports::add_log("palette #" + std::to_string(number) + " has an unexpected size");
                    return;
                }
                std::memcpy(r.palette_pixels.data() + size_t(pid.index()) * palette_width * palette_height * 3, im.px, size_t(palette_width) * palette_height * 3);
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, pid.index(), palette_width, palette_height, 1, GL_RGB, GL_UNSIGNED_BYTE, im.px);
            });
            image fx;
            if (load_image(s.fs, dir.get_single_file("effectcolors.png"), 3, fx))
                r.effects_texture = make_texture_2d(fx, GL_RGB, GL_RGB8);
            else
                reports::add_log("missing palettes/effectcolors.png");
        }

        // DressMySlugcat style atlas, transcribed into slugcat.txt
        void load_slugcat_atlas(state& r, layered_fs::filesystem const& fs) {
            image im;
            auto const root = fs.get_root();
            if (!load_image(fs, root.get_single_file("slugcat.png"), 4, im)) {
                reports::add_log("missing slugcat.png, slugcats are drawn as circles");
                return;
            }
            r.slugcat_texture = make_texture_2d(im, GL_RGBA, GL_RGBA8);
            r.slugcat_width = im.w;
            r.slugcat_height = im.h;
            std::string const txt = read_text(fs, root.get_single_file("slugcat.txt"));
            size_t pos = 0;
            while (pos < txt.size()) {
                size_t const eol = std::min(txt.find('\n', pos), txt.size());
                std::string const line = txt.substr(pos, eol - pos);
                pos = eol + 1;
                if (line.empty() || line[0] == '#')
                    continue;
                char name[64];
                atlas_frame f;
                if (std::sscanf(line.c_str(), "%63s %f %f %f %f", name, &f.x, &f.y, &f.w, &f.h) == 5)
                    r.slugcat_frames.emplace(name, f);
            }
        }
    }

    void load_assets(state& r, sys::game_state const& s) {
        load_font(r, s.fs);
        load_room_shader(r, s.fs);
        load_palettes(r, s);
        load_slugcat_atlas(r, s.fs);
        load_warp_shaders(r, s.fs);
    }

    // Camera images of the room as one texture array, replacing the last one
    void use_room(state& r, sys::game_state const& s, dcon::room_id rid) {
        if (r.room_texture_room == rid)
            return;
        auto const& w = s.world;
        if (r.room_texture != 0) {
            glDeleteTextures(1, &r.room_texture);
            r.room_texture = 0;
        }
        r.room_texture_room = rid;
        r.room_texture_layers = 0;
        if (r.room_program == 0)
            return;
        std::string const name{s.to_string_view(w.room_get_name(rid))};
        auto const region = w.room_get_region_from_region_membership(rid);
        std::string const region_name{s.to_string_view(w.region_get_name(region))};
        auto const world_dir = s.fs.get_root().get_single_subdir("world");
        auto const dir = w.region_get_is_pseudo(region) ? world_dir.get_single_subdir(region_name) : world_dir.get_single_subdir(region_name + "-rooms");
        auto const range = w.room_range_of_camera_location(rid);
        uint32_t const cams = uint32_t(range.second - range.first);
        if (cams == 0)
            return;
        glGenTextures(1, &r.room_texture);
        glBindTexture(GL_TEXTURE_2D_ARRAY, r.room_texture);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGB8, camera_width, camera_height, GLsizei(cams), 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        uint32_t loaded = 0;
        for (uint32_t i = 0; i < cams; ++i) {
            image im;
            auto const f = dir.get_single_file(name + "_" + std::to_string(i + 1) + ".png");
            if (!load_image(s.fs, f, 3, im)) {
                reports::add_log("missing camera image \"" + f.path + "\"");
                continue;
            }
            if (im.w != camera_width || im.h != camera_height) {
                reports::add_log("unexpected camera image size for \"" + f.path + "\"");
                continue;
            }
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, GLint(i), camera_width, camera_height, 1, GL_RGB, GL_UNSIGNED_BYTE, im.px);
            ++loaded;
        }
        r.room_texture_layers = loaded == cams ? cams : 0;
    }

    void shutdown(state& r) {
        if (r.context != nullptr) {
            glDeleteBuffers(1, &r.vbo);
            glDeleteVertexArrays(1, &r.vao);
            glDeleteProgram(r.program);
            glDeleteTextures(1, &r.font_texture);
            glDeleteTextures(1, &r.palette_texarray);
            glDeleteTextures(1, &r.effects_texture);
            glDeleteTextures(1, &r.slugcat_texture);
            glDeleteTextures(1, &r.room_texture);
            glDeleteProgram(r.room_program);
            SDL_GL_DestroyContext(r.context);
        }
        if (r.window != nullptr)
            SDL_DestroyWindow(r.window);
        SDL_Quit();
        r = state{};
    }

    rw::vec2 screen_to_room(state const& r, camera const& cam, float sx, float sy) {
        return vec2{sx / cam.zoom + cam.pos.x, (float(r.height) - sy) / cam.zoom + cam.pos.y};
    }

    namespace {
        void upload_and_draw(state& r, size_t first, size_t count) {
            if (count > 0)
                glDrawArrays(GL_TRIANGLES, GLint(first), GLsizei(count));
        }

        // Debug view of the collision geometry
        void build_geometry(state& r, sys::game_state const& s, world::room_view const& room, camera const& cam, float view_w, float view_h, uint8_t alpha) {
            auto const& world = s.world;
            painter p{r.batch};
            auto const fade = [alpha](uint32_t c) { return (c & 0x00FFFFFFu) | (uint32_t((uint32_t(c >> 24) * alpha) / 255u) << 24); };
            int32_t const x0 = std::max(0, int32_t(std::floor(cam.pos.x / 20.f)) - 1);
            int32_t const y0 = std::max(0, int32_t(std::floor(cam.pos.y / 20.f)) - 1);
            int32_t const x1 = std::min(room.width - 1, int32_t(std::ceil((cam.pos.x + view_w) / 20.f)) + 1);
            int32_t const y1 = std::min(room.height - 1, int32_t(std::ceil((cam.pos.y + view_h) / 20.f)) + 1);
            // layers in painter's order: background walls, terrain, details
            for (int pass = 0; pass < 3; ++pass) {
                for (int32_t y = y0; y <= y1; ++y) {
                    for (int32_t x = x0; x <= x1; ++x) {
                        auto const t = room.get_tile(x, y);
                        float const fx = float(x) * 20.f;
                        float const fy = float(y) * 20.f;
                        if (pass == 0) {
                            if (t.flags & world::tile_wall_behind)
                                p.rect(fx, fy, 20.f, 20.f, fade(col_wall_behind));
                        } else if (pass == 1) {
                            if (t.t == terrain::solid) {
                                p.rect(fx, fy, 20.f, 20.f, fade(col_solid));
                            } else if (t.t == terrain::slope) {
                                // triangle filling the corner between the two solid neighbours
                                vec2 const bl{fx, fy}, br{fx + 20.f, fy}, tl{fx, fy + 20.f}, tr{fx + 20.f, fy + 20.f};
                                uint32_t const c = fade(col_solid);
                                switch (room.identify_slope(x, y)) {
                                case world::slope_direction::up_right: p.tri(bl, br, tl, c); break;   // solid left & below
                                case world::slope_direction::up_left: p.tri(bl, br, tr, c); break;    // solid right & below
                                case world::slope_direction::down_right: p.tri(tl, tr, bl, c); break; // solid left & above
                                case world::slope_direction::down_left: p.tri(tl, tr, br, c); break;  // solid right & above
                                default: p.tri(bl, br, vec2{fx + 10.f, fy + 10.f}, c); break;
                                }
                            }
                        } else {
                            if (t.t == terrain::floor)
                                p.rect(fx, fy + 15.f, 20.f, 5.f, fade(col_floor));
                            if (t.vertical_beam())
                                p.rect(fx + 8.5f, fy, 3.f, 20.f, fade(col_beam));
                            if (t.horizontal_beam())
                                p.rect(fx, fy + 8.5f, 20.f, 3.f, fade(col_beam));
                            if (t.shortcut != 0 && t.t != terrain::shortcut_entrance)
                                p.rect(fx + 7.f, fy + 7.f, 6.f, 6.f, fade(col_shortcut_path));
                            if (t.t == terrain::shortcut_entrance)
                                p.rect(fx + 2.f, fy + 2.f, 16.f, 16.f, fade(col_shortcut_entrance));
                        }
                    }
                }
            }
            float const line_w = 1.f / cam.zoom;
            p.outline(0.f, 0.f, room.pixel_width(), room.pixel_height(), line_w, col_bounds);
            if (r.show_cameras) {
                world.room_for_each_camera_location(room.id, [&](dcon::camera_location_id cl) {
                    vec2 const cp = world.room_camera_get_position(world.camera_location_get_camera(cl));
                    p.outline(cp.x, cp.y, 1366.f, 768.f, line_w, col_camera);
                });
            }
        }

        uint32_t to_rgba(rw::vec3 c, float a = 1.f) {
            auto const b = [](float v) { return uint8_t(mathf::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            return rgba(b(c.x), b(c.y), b(c.z), b(a));
        }

        // PlayerGraphics sprites through the slugcat atlas (textured batch)
        void build_slugcats(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            auto const& world = s.world;
            static std::vector<player_graphics::draw_cmd> cmds;
            painter p{r.batch};
            float const tw = float(r.slugcat_width);
            float const th = float(r.slugcat_height);
            auto const white_it = r.slugcat_frames.find("pixel");
            vec2 white_uv{0.f, 0.f};
            if (white_it != r.slugcat_frames.end())
                white_uv = vec2{(white_it->second.x + white_it->second.w * 0.5f) / tw, (white_it->second.y + white_it->second.h * 0.5f) / th};
            world.for_each_player([&](dcon::player_id pid) {
                auto const o = world.player_get_body_from_player_body(pid);
                if (world.physical_object_get_room_from_object_location(o) != room.id || world.player_get_in_shortcut(pid) || world.player_get_eaten(pid))
                    return;
                cmds.clear();
                player_graphics::build(s, pid, alpha, cmds);
                // the Watcher's camouflage: a plain fade out (vanilla masks it into the background)
                float const camo = mathf::lerp(world.player_get_last_camo_progress(pid), world.player_get_camo_progress(pid), alpha);
                float const vis = 1.f - 0.88f * camo;
                for (auto const& c : cmds) {
                    if (c.is_triangle) {
                        uint32_t const col = to_rgba(c.color, vis);
                        for (int k = 0; k < 3; ++k) {
                            uint32_t const vc = c.vertex_colors ? to_rgba(rw::vec3{c.vcol[k].x, c.vcol[k].y, c.vcol[k].z}, c.vcol[k].w * vis) : col;
                            r.batch.push_back(vertex{c.tri[k].x, c.tri[k].y, vc, white_uv.x, white_uv.y});
                        }
                        continue;
                    }
                    auto const it = r.slugcat_frames.find(std::string{c.spr.element});
                    if (it == r.slugcat_frames.end())
                        continue;
                    auto const& f = it->second;
                    // Futile: rotation is clockwise, anchor relative to the element
                    float const rad = c.spr.rotation * mathf::deg2rad;
                    float const cs = std::cos(rad), sn = std::sin(rad);
                    float const lx0 = -c.spr.anchor.x * f.w * c.spr.scale.x, lx1 = (1.f - c.spr.anchor.x) * f.w * c.spr.scale.x;
                    float const ly0 = -c.spr.anchor.y * f.h * c.spr.scale.y, ly1 = (1.f - c.spr.anchor.y) * f.h * c.spr.scale.y;
                    auto const place = [&](float lx, float ly) { return c.spr.pos + vec2{lx * cs + ly * sn, -lx * sn + ly * cs}; };
                    uint32_t const col = to_rgba(c.spr.color, vis);
                    float const u0 = f.x / tw, u1 = (f.x + f.w) / tw;
                    float const v_top = f.y / th, v_bottom = (f.y + f.h) / th;
                    vertex const bl{place(lx0, ly0).x, place(lx0, ly0).y, col, u0, v_bottom};
                    vertex const br{place(lx1, ly0).x, place(lx1, ly0).y, col, u1, v_bottom};
                    vertex const tr{place(lx1, ly1).x, place(lx1, ly1).y, col, u1, v_top};
                    vertex const tl{place(lx0, ly1).x, place(lx0, ly1).y, col, u0, v_top};
                    r.batch.insert(r.batch.end(), {bl, br, tr, bl, tr, tl});
                }
            });
            (void)p;
        }

        // fallback when there is no slugcat atlas
        void build_slugcat_circles(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            auto const& world = s.world;
            painter p{r.batch};
            world.for_each_player([&](dcon::player_id pid) {
                auto const o = world.player_get_body_from_player_body(pid);
                if (world.physical_object_get_room_from_object_location(o) != room.id || world.player_get_in_shortcut(pid) || world.player_get_eaten(pid))
                    return;
                auto const c0 = physics::chunk(s, o, 0);
                auto const c1 = physics::chunk(s, o, 1);
                vec2 const b0 = rw::lerp(world.body_chunk_get_last_pos(c0), world.body_chunk_get_pos(c0), alpha);
                vec2 const b1 = rw::lerp(world.body_chunk_get_last_pos(c1), world.body_chunk_get_pos(c1), alpha);
                uint32_t const body = world.player_get_dead(pid) ? col_body_dead : to_rgba(world.player_get_stats(pid).body_color);
                p.line(b0, b1, 2.f, body);
                p.circle(b1, world.body_chunk_get_rad(c1), body);
                p.circle(b0, world.body_chunk_get_rad(c0), body);
                p.circle(b0 + vec2{float(world.player_get_flip_direction(pid)) * 4.f, 3.f}, 2.f, col_eye);
            });
        }
    }

    namespace {
        // Rock.DrawSprites: a pebble in the palette's black, plus the
        // triangle trail while thrown; Spear.DrawSprites
        // `behind`: only the items drawn behind creatures (Hunter's back spear),
        // else every other one
        bool draw_new_item(painter& p, sys::game_state const& s, dcon::item_id it, world::item_type type, vec2 pos, float alpha);

        void build_items(state& r, sys::game_state const& s, world::room_view const& room, float alpha, bool behind) {
            auto const& world = s.world;
            painter p{r.batch};
            world.for_each_item([&](dcon::item_id it) {
                auto const o = items::body(s, it);
                if (world.physical_object_get_room_from_object_location(o) != room.id)
                    return;
                auto const g = items::grabber(s, it);
                if (g && world.player_get_in_shortcut(g))
                    return;
                if ((world.item_get_mode(it) == world::weapon_mode::on_back) != behind)
                    return;
                auto const c = items::chunk(s, it);
                vec2 const pos = rw::lerp(world.body_chunk_get_last_pos(c), world.body_chunk_get_pos(c), alpha);
                auto const type = world.item_get_type(it);
                // hanging or growing: its stalk first
                if (auto const anchor = world.item_get_stalk(it); anchor.has_value() && world.item_get_stalk_length(it) > 0.f)
                    p.line(*anchor, pos, 1.2f, rgba(40, 52, 30));
                if (draw_new_item(p, s, it, type, pos, alpha))
                    return;
                if (type == world::item_type::spear) {
                    // SmallSpear: a thin shaft with a slightly wider head
                    vec2 tail, tip;
                    items::spear_ends(s, it, alpha, tail, tip);
                    vec2 const d = rw::normalized(tip - tail);
                    vec2 const n = rw::custom::perpendicular_vector(d);
                    uint32_t const col = world.item_get_needle(it) ? col_needle : col_rock;
                    p.line(tail, tip - d * 6.f, 2.f, col);
                    p.tri(tip, tip - d * 8.f + n * 2.f, tip - d * 8.f - n * 2.f, col);
                    return;
                }
                vec2 const rot = rw::normalized(rw::lerp(world.item_get_last_rotation(it), world.item_get_rotation(it), alpha));
                vec2 const side = rw::custom::perpendicular_vector(rot);
                // an irregular pentagon, rotated with the rock
                constexpr float shape[5][2] = {{0.f, 5.5f}, {4.5f, 2.f}, {3.5f, -4.f}, {-3.f, -4.5f}, {-5.f, 1.5f}};
                for (int i = 0; i < 5; ++i) {
                    int const j = (i + 1) % 5;
                    vec2 const a = pos + rot * shape[i][1] + side * shape[i][0];
                    vec2 const b = pos + rot * shape[j][1] + side * shape[j][0];
                    p.tri(pos, a, b, col_rock);
                }
                if (world.item_get_mode(it) == world::weapon_mode::thrown) {
                    vec2 const tail = world.body_chunk_get_last_pos(c);
                    vec2 const n = rw::custom::perpendicular_vector(rw::normalized(pos - tail));
                    p.tri(pos + n * 3.f, pos - n * 3.f, tail, col_rock);
                }
            });
        }

        // the items beyond rocks and plain spears, with simple shapes of our own
        bool draw_new_item(painter& p, sys::game_state const& s, dcon::item_id it, world::item_type type, vec2 pos, float alpha) {
            auto const& w = s.world;
            using world::item_type;
            vec2 const rot = rw::normalized(rw::lerp(w.item_get_last_rotation(it), w.item_get_rotation(it), alpha));
            vec2 const side = rw::custom::perpendicular_vector(rot);
            auto const ellipse = [&](vec2 c, vec2 a, vec2 b, uint32_t col) {
                constexpr int segs = 14;
                for (int k = 0; k < segs; ++k) {
                    float const t0 = float(k) / segs * 6.2831853f, t1 = float(k + 1) / segs * 6.2831853f;
                    p.tri(c, c + a * std::cos(t0) + b * std::sin(t0), c + a * std::cos(t1) + b * std::sin(t1), col);
                }
            };
            switch (type) {
            case item_type::explosive_spear:
            case item_type::electric_spear: {
                vec2 tail, tip;
                items::spear_ends(s, it, alpha, tail, tip);
                vec2 const d = rw::normalized(tip - tail);
                vec2 const n = rw::custom::perpendicular_vector(d);
                p.line(tail, tip - d * 6.f, 2.f, col_rock);
                p.tri(tip, tip - d * 8.f + n * 2.f, tip - d * 8.f - n * 2.f, col_rock);
                if (type == item_type::explosive_spear) {
                    // a charge lashed behind the head, with a red band
                    vec2 const at = tip - d * 13.f;
                    ellipse(at, d * 4.f, n * 3.2f, rgba(70, 36, 30));
                    p.line(at - d * 1.f + n * 3.2f, at - d * 1.f - n * 3.2f, 1.5f, rgba(200, 50, 30));
                } else {
                    // copper coils, glowing while it holds charge
                    bool const charged = w.item_get_charge(it) > 0;
                    for (int k = 0; k < 3; ++k) {
                        vec2 const at = tip - d * (11.f + float(k) * 3.f);
                        p.line(at + n * 2.6f, at - n * 2.6f, 1.3f, charged ? rgba(190, 120, 60) : rgba(90, 70, 55));
                    }
                    if (charged)
                        p.glow(tip - d * 4.f, 9.f, rgba(150, 200, 255, 140), rgba(150, 200, 255, 0));
                }
                return true;
            }
            case item_type::bomb:
                ellipse(pos, rot * 5.5f, side * 5.f, rgba(46, 40, 38));
                p.line(pos + rot * 5.f, pos + rot * 9.f, 1.3f, rgba(120, 100, 70));
                p.circle(pos + rot * 1.5f + side * 1.5f, 1.2f, rgba(110, 100, 95));
                return true;
            case item_type::flare_bomb:
                p.glow(pos, 12.f, rgba(255, 250, 220, 90), rgba(255, 250, 220, 0));
                ellipse(pos, rot * 4.5f, side * 4.5f, rgba(225, 222, 200));
                p.circle(pos, 2.f, rgba(255, 255, 245));
                return true;
            case item_type::lilly_puck:
                // a thin disc seen on its edge as it turns
                ellipse(pos, rot * 7.f, side * 2.2f, rgba(120, 170, 110));
                p.line(pos - rot * 6.f, pos + rot * 6.f, 0.8f, rgba(200, 230, 180));
                return true;
            case item_type::mushroom: {
                // a pale stem under a domed cap, upright while it grows
                vec2 const up = w.item_get_stalk(it).has_value() ? vec2{0.f, 1.f} : rot;
                vec2 const across = rw::custom::perpendicular_vector(up);
                p.line(pos - up * 4.f, pos + up * 3.f, 2.f, rgba(225, 220, 205));
                ellipse(pos + up * 4.f, across * 5.f, up * 3.f, rgba(240, 236, 228));
                p.circle(pos + up * 5.f + across * 2.f, 0.8f, rgba(200, 190, 175));
                return true;
            }
            case item_type::slime_mold: {
                float const g = w.item_get_glow(it);
                p.glow(pos, 16.f, rgba(255, 150, 40, uint8_t(80 * g)), rgba(255, 150, 40, 0));
                ellipse(pos, vec2{6.f, 0.f}, vec2{0.f, 4.5f}, rgba(240, 110, 30));
                p.circle(pos + vec2{-1.5f, 1.5f}, 1.5f, rgba(255, 190, 110));
                return true;
            }
            case item_type::dangle_fruit:
            case item_type::rotten_fruit: {
                bool const rotten = type == item_type::rotten_fruit;
                ellipse(pos, vec2{5.f, 0.f}, vec2{0.f, 6.f}, rotten ? rgba(70, 34, 70) : rgba(60, 90, 210));
                if (rotten) {
                    p.circle(pos + vec2{1.5f, -1.f}, 1.6f, rgba(30, 18, 30));
                    p.circle(pos + vec2{-2.f, 2.f}, 1.1f, rgba(120, 60, 110));
                } else {
                    p.circle(pos + vec2{-1.6f, 2.f}, 1.4f, rgba(150, 180, 255));
                }
                return true;
            }
            case item_type::data_pearl: {
                rw::vec3 const c = w.item_get_color(it);
                if (w.item_get_glow(it) > 0.f)
                    p.glow(pos, 14.f, to_rgba(c, 0.5f), to_rgba(c, 0.f));
                p.circle(pos, 4.f, to_rgba(c * 0.75f));
                p.circle(pos, 3.f, to_rgba(c));
                p.circle(pos + vec2{-1.2f, 1.2f}, 1.f, rgba(255, 255, 255, 230));
                return true;
            }
            default:
                return false;
            }
        }

        // SeedCob plants: a stalk from the ground up to the cob, swaying a little.
        // Closed cobs show their rows of seeds, eaten (dead) ones only their open
        // husk, rotten ones a dark swollen pod
        void build_plants(state& r, sys::game_state const& s, world::room_view const& room, float seconds) {
            auto const& w = s.world;
            painter p{r.batch};
            w.room_for_each_thing_location(room.id, [&](dcon::thing_location_id l) {
                auto const t = w.thing_location_get_thing(l);
                auto const kind = w.placed_thing_get_kind(t);
                if (kind != world::thing_kind::seed_cob && kind != world::thing_kind::dead_seed_cob && kind != world::thing_kind::rotten_seed_cob)
                    return;
                vec2 const head = w.placed_thing_get_pos(t);
                float ground = head.y - 120.f;
                for (float down = 10.f; down < 600.f; down += 10.f)
                    if (room.get_tile(head - vec2{0.f, down}).solid()) {
                        ground = world::room_view::middle_of_tile(head - vec2{0.f, down}).y + 10.f;
                        break;
                    }
                vec2 const root{head.x, ground};
                float const sway = std::sin(seconds * 0.9f + head.x * 0.01f) * 4.f;
                vec2 const top = head + vec2{sway, 0.f};
                vec2 const mid = rw::lerp(root, top, 0.5f) + vec2{sway * 0.3f + 6.f, 0.f};
                uint32_t const stalk = kind == world::thing_kind::rotten_seed_cob ? rgba(46, 30, 44) : rgba(60, 70, 40);
                vec2 prev = root;
                for (int k = 1; k <= 12; ++k) {
                    float const q = float(k) / 12.f;
                    vec2 const pt = rw::lerp(rw::lerp(root, mid, q), rw::lerp(mid, top, q), q);
                    p.line(prev, pt, mathf::lerp(4.f, 2.f, q), stalk);
                    prev = pt;
                }
                vec2 const up = rw::normalized(top - mid);
                vec2 const across = rw::custom::perpendicular_vector(up);
                vec2 const c = top + up * 18.f;
                auto const pod = [&](vec2 ctr, float len, float wid, uint32_t col) {
                    constexpr int segs = 16;
                    for (int k = 0; k < segs; ++k) {
                        float const a0 = float(k) / segs * 6.2831853f, a1 = float(k + 1) / segs * 6.2831853f;
                        p.tri(ctr, ctr + up * std::cos(a0) * len + across * std::sin(a0) * wid, ctr + up * std::cos(a1) * len + across * std::sin(a1) * wid, col);
                    }
                };
                switch (kind) {
                case world::thing_kind::seed_cob:
                    pod(c, 20.f, 7.f, rgba(70, 82, 44));
                    for (int row = -2; row <= 2; ++row)
                        for (int k = -4; k <= 4; ++k)
                            p.circle(c + up * (float(k) * 3.8f) + across * (float(row) * 2.3f), 1.3f, rgba(225, 190, 80));
                    break;
                case world::thing_kind::dead_seed_cob:
                    // the husk split in two, curled back and empty
                    for (int sgn = -1; sgn <= 1; sgn += 2) {
                        vec2 const o = across * (float(sgn) * 6.f);
                        p.line(c - up * 16.f, c + o + up * 4.f, 2.5f, rgba(110, 90, 55));
                        p.line(c + o + up * 4.f, c + o * 1.6f + up * 16.f, 2.f, rgba(130, 105, 65));
                    }
                    p.line(c - up * 16.f, c + up * 14.f, 2.f, rgba(90, 76, 50));
                    break;
                default:
                    pod(c, 21.f, 8.5f, rgba(48, 22, 50));
                    for (int k = 0; k < 7; ++k) {
                        float const a = float(k) * 2.4f;
                        p.circle(c + up * (std::sin(a) * 14.f) + across * (std::cos(a) * 5.f), 2.f, rgba(110, 40, 120));
                    }
                    break;
                }
            });
        }

        // the sand: shaded by its slope, with fine wind ripples, its far side
        // a lighter band above the front edge
        void build_sand(state& r, sys::game_state const& s, world::room_view const& room, camera const& cam) {
            auto const* sand = ::terrain::sand_if_built(s, room.id);
            if (sand == nullptr)
                return;
            float const view_w = float(r.width) / cam.zoom;
            float const x0 = mathf::max(cam.pos.x - 20.f, sand->left), x1 = mathf::min(cam.pos.x + view_w + 20.f, sand->right), bottom = cam.pos.y - 40.f;
            rw::vec3 const front{0.62f, 0.5f, 0.34f}, top_col{0.8f, 0.68f, 0.48f}, dark{0.36f, 0.28f, 0.2f};
            constexpr float step = 4.f;
            for (float x = x0; x < x1; x += step) {
                float const ha = ::terrain::sand_height(*sand, x), hb = ::terrain::sand_height(*sand, x + step);
                float const slope = (hb - ha) / step;
                float const ripple = std::sin(x * 0.21f + ha * 0.35f) * 0.04f;
                float const lit = mathf::clamp01(0.75f - slope * 0.6f + ripple);
                uint32_t const face = to_rgba(dark + (front - dark) * lit);
                uint32_t const deep = to_rgba(dark * 0.8f);
                r.batch.push_back(vertex{x, ha, face, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, hb, face, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, bottom, deep, 0.f, 0.f});
                r.batch.push_back(vertex{x, ha, face, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, bottom, deep, 0.f, 0.f});
                r.batch.push_back(vertex{x, bottom, deep, 0.f, 0.f});
                float const ba = mathf::min(::terrain::sand_back(*sand, x), 40.f) * 0.5f + 4.f;
                float const bb = mathf::min(::terrain::sand_back(*sand, x + step), 40.f) * 0.5f + 4.f;
                uint32_t const crest = to_rgba(top_col * (0.85f + 0.15f * lit), 0.95f);
                r.batch.push_back(vertex{x, ha, crest, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, hb, crest, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, hb + bb, crest, 0.f, 0.f});
                r.batch.push_back(vertex{x, ha, crest, 0.f, 0.f});
                r.batch.push_back(vertex{x + step, hb + bb, crest, 0.f, 0.f});
                r.batch.push_back(vertex{x, ha + ba, crest, 0.f, 0.f});
            }
        }

        // MudPit: thick brown, its surface heaving slowly
        void build_mud(state& r, sys::game_state const& s, world::room_view const& room, float seconds) {
            auto const& w = s.world;
            painter p{r.batch};
            w.room_for_each_thing_location(room.id, [&](dcon::thing_location_id l) {
                auto const t = w.thing_location_get_thing(l);
                if (w.placed_thing_get_kind(t) != world::thing_kind::mud_pit)
                    return;
                vec2 const pos = w.placed_thing_get_pos(t);
                vec2 const size = w.placed_thing_get_handle(t);
                float const top = pos.y + size.y;
                float const bottom = pos.y - mathf::max(10.f, w.placed_thing_get_extra(t));
                float const xa = mathf::min(pos.x, pos.x + size.x), xb = mathf::max(pos.x, pos.x + size.x);
                for (float x = xa; x < xb; x += 4.f) {
                    float const x2 = mathf::min(x + 4.f, xb);
                    auto const surf = [&](float xx) { return top + std::sin(xx * 0.05f + seconds * 0.7f) * 1.5f + std::sin(xx * 0.13f - seconds * 0.4f) * 0.8f; };
                    uint32_t const c0 = rgba(70, 48, 30, 245), c1 = rgba(38, 26, 18, 250);
                    r.batch.push_back(vertex{x, surf(x), c0, 0.f, 0.f});
                    r.batch.push_back(vertex{x2, surf(x2), c0, 0.f, 0.f});
                    r.batch.push_back(vertex{x2, bottom, c1, 0.f, 0.f});
                    r.batch.push_back(vertex{x, surf(x), c0, 0.f, 0.f});
                    r.batch.push_back(vertex{x2, bottom, c1, 0.f, 0.f});
                    r.batch.push_back(vertex{x, bottom, c1, 0.f, 0.f});
                    p.line(vec2{x, surf(x)}, vec2{x2, surf(x2)}, 1.2f, rgba(105, 78, 50, 220));
                }
            });
        }

        // Custom.HSL2RGB
        rw::vec3 hsl(float h, float s, float l) {
            h = h - std::floor(h);
            auto const f = [&](float n) {
                float const k = std::fmod(n + h * 12.f, 12.f);
                float const a = s * std::min(l, 1.f - l);
                return l - a * std::max(-1.f, std::min({k - 3.f, 9.f - k, 1.f}));
            };
            return rw::vec3{f(0.f), f(8.f), f(4.f)};
        }

        // Centipedes, with shapes of our own: a chain of overlapping shell
        // plates (dark rim, coloured plate, a lighter ridge catching the light)
        // over a pale belly line, three jointed legs a side per plate reaching
        // for the nearest surface and stepping with the walk cycle, curved
        // feelers and a pair of pincers at both ends, the blue flash of a shock
        void build_centipedes(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            auto const& w = s.world;
            painter p{r.batch};
            w.for_each_centipede([&](dcon::centipede_id c) {
                if (w.physical_object_get_room_from_object_location(centipede::body(s, c)) != room.id)
                    return;
                uint32_t const n = centipede::chunk_count(s, c);
                if (n == 0)
                    return;
                float const size = w.centipede_get_size(c);
                bool const dead = w.centipede_get_dead(c);
                bool const stunned = w.centipede_get_stun(c) > 0;
                auto const at = [&](uint32_t i) {
                    auto const k = centipede::chunk(s, c, i);
                    return rw::lerp(w.body_chunk_get_last_pos(k), w.body_chunk_get_pos(k), alpha);
                };
                auto const dir_at = [&](uint32_t i) {
                    return rw::normalized(at(std::min(i + 1, n - 1)) - at(i > 0 ? i - 1 : 0));
                };
                float const hue = w.centipede_get_hue(c);
                float const sat = dead ? 0.25f : mathf::lerp(0.6f, 0.95f, size);
                // earthy: the centipede's hue pulled well towards brown
                rw::vec3 const brown{0.42f, 0.27f, 0.15f};
                auto const earthy = [&](rw::vec3 c, float k) { return c + (brown * (0.6f + 0.8f * (c.x + c.y + c.z) / 3.f) - c) * k; };
                rw::vec3 const chitin{0.09f, 0.065f, 0.05f};
                rw::vec3 const plate = earthy(hsl(hue, sat, dead ? 0.28f : 0.42f), 0.5f);
                rw::vec3 const ridge = earthy(hsl(hue, sat * 0.8f, dead ? 0.38f : 0.62f), 0.4f);
                rw::vec3 const belly = earthy(hsl(hue + 0.05f, sat * 0.4f, 0.2f), 0.6f);
                float const cycle = w.centipede_get_walk_cycle(c);
                float const scale = mathf::lerp(0.75f, 1.35f, size);
                auto const oval = [&](vec2 ctr, vec2 d, vec2 sd, float a, float b, uint32_t col) {
                    constexpr int segs = 12;
                    for (int k = 0; k < segs; ++k) {
                        float const a0 = float(k) / segs * 6.2831853f, a1 = float(k + 1) / segs * 6.2831853f;
                        p.tri(ctr, ctr + d * (std::cos(a0) * a) + sd * (std::sin(a0) * b), ctr + d * (std::cos(a1) * a) + sd * (std::sin(a1) * b), col);
                    }
                };

                // legs under everything
                for (uint32_t i = 0; i < n; ++i) {
                    vec2 const pos = at(i);
                    vec2 const dir = dir_at(i);
                    vec2 const side = rw::custom::perpendicular_vector(dir);
                    float const t = float(i) / float(std::max<uint32_t>(n - 1, 1));
                    float const len = mathf::lerp(12.f, 20.f, std::sin(t * mathf::pi)) * scale;
                    for (int l = -1; l <= 1; l += 2) {
                        float const step = dead ? 0.f : std::sin(cycle + float(i) * 1.1f + (l > 0 ? mathf::pi : 0.f));
                        // reach for a surface on that side, else hang down
                        vec2 foot = pos + side * (float(l) * len * 0.8f) + vec2{0.f, -len * 0.5f};
                        for (int k = 1; k <= 5; ++k) {
                            vec2 const probe = pos + side * (float(l) * len * float(k) / 5.f) + vec2{0.f, -len * 0.15f * float(k) / 5.f};
                            if (room.get_tile(probe).solid()) {
                                foot = probe;
                                break;
                            }
                        }
                        foot += dir * (step * len * 0.3f);
                        if (dead || stunned)
                            foot = pos + side * (float(l) * len * 0.5f) + dir * (std::sin(float(i) * 2.3f) * len * 0.3f);
                        // three pieces bending away from the body
                        vec2 const hip = pos + side * (float(l) * 2.f * scale);
                        vec2 const knee = rw::lerp(hip, foot, 0.4f) + side * (float(l) * len * 0.35f) + dir * (len * 0.12f);
                        vec2 const ankle = rw::lerp(knee, foot, 0.6f) + side * (float(l) * len * 0.08f);
                        uint32_t const leg = to_rgba(chitin + (plate - chitin) * 0.25f);
                        p.line(hip, knee, 1.8f * scale, leg);
                        p.line(knee, ankle, 1.3f * scale, leg);
                        p.line(ankle, foot, 0.9f * scale, to_rgba(chitin));
                    }
                }
                // feelers and pincers at both ends
                for (int end = 0; end < 2; ++end) {
                    uint32_t const i = end == 0 ? 0 : n - 1;
                    uint32_t const j = end == 0 ? std::min(1u, n - 1) : (n > 1 ? n - 2 : 0);
                    vec2 const dir = rw::normalized(at(i) - at(j));
                    vec2 const side = rw::custom::perpendicular_vector(dir);
                    float const rad = w.body_chunk_get_rad(centipede::chunk(s, c, i));
                    for (int l = -1; l <= 1; l += 2) {
                        float const wave = dead ? 0.f : std::sin(cycle * 0.7f + float(l) + float(end) * 1.7f) * 0.25f;
                        vec2 prev = at(i) + dir * rad * 0.8f + side * (float(l) * rad * 0.4f);
                        vec2 d = rw::normalized(dir + side * (float(l) * (0.45f + wave)));
                        for (int k = 0; k < 5; ++k) {
                            d = rw::normalized(d + side * (float(l) * 0.12f) - dir * 0.05f);
                            vec2 const next = prev + d * (4.f * scale);
                            p.line(prev, next, mathf::lerp(1.1f, 0.5f, float(k) / 4.f), to_rgba(chitin));
                            prev = next;
                        }
                        vec2 const base = at(i) + dir * rad * 0.9f + side * (float(l) * rad * 0.5f);
                        vec2 const tip = base + dir * (5.f * scale) - side * (float(l) * 3.f * scale);
                        p.line(base, base + dir * (3.f * scale) + side * (float(l) * 1.5f * scale), 1.6f * scale, to_rgba(chitin));
                        p.line(base + dir * (3.f * scale) + side * (float(l) * 1.5f * scale), tip, 1.2f * scale, to_rgba(chitin));
                    }
                }
                // the belly line, then the plates from tail to head so they overlap
                for (uint32_t i = 0; i + 1 < n; ++i)
                    p.line(at(i), at(i + 1), mathf::max(1.5f, w.body_chunk_get_rad(centipede::chunk(s, c, i)) * 0.9f), to_rgba(belly));
                for (uint32_t q = 0; q < n; ++q) {
                    uint32_t const i = n - 1 - q;
                    vec2 const pos = at(i);
                    vec2 const dir = dir_at(i);
                    vec2 const side = rw::custom::perpendicular_vector(dir);
                    float const rad = w.body_chunk_get_rad(centipede::chunk(s, c, i));
                    // the side facing up catches the light
                    vec2 const up = side.y >= 0.f ? side : -side;
                    float const shimmer = dead ? 0.f : 0.08f * std::sin(cycle * 0.5f + float(i) * 0.9f);
                    oval(pos, dir, side, rad * 1.45f, rad * 1.2f, to_rgba(chitin));
                    oval(pos + up * (rad * 0.08f), dir, side, rad * 1.25f, rad * 1.0f, to_rgba(plate));
                    oval(pos + up * (rad * 0.4f), dir, side, rad * 0.9f, rad * 0.35f, to_rgba(ridge * (1.f + shimmer)));
                    // the seam to the next plate
                    p.line(pos - dir * (rad * 1.1f) + side * (rad * 0.9f), pos - dir * (rad * 1.1f) - side * (rad * 0.9f), 0.8f, to_rgba(chitin));
                }
                float const flash = w.centipede_get_light_flash(c);
                if (flash > 0.f) {
                    vec2 const head = at(centipede::head_index(s, c));
                    p.glow(head, 60.f * flash + 20.f, to_rgba(rw::vec3{0.7f, 0.7f, 1.f}, 0.8f * flash), to_rgba(rw::vec3{0.7f, 0.7f, 1.f}, 0.f));
                }
            });
        }

        // DaddyCorruption's sprites, with plain shapes. `front`: what vanilla
        // puts in the Water/Shortcuts containers over the level (most bulbs,
        // eye slits, dots, black goo, CorruptionDarkness), else the Midground
        // (tubes and the bulbs among creatures)
        struct rot_palette {
            rw::vec3 black{0.f, 0.f, 0.f};
            rw::vec3 effect_a{1.f, 1.f, 1.f};
            rw::vec3 effect_b{1.f, 1.f, 1.f};
        };
        rot_palette room_rot_palette(state const& r, sys::game_state const& s, dcon::room_id room) {
            rot_palette pal;
            auto const palette = s.world.room_get_palette(room).id;
            if (!palette || r.palette_pixels.empty())
                return pal;
            // Texture2D.GetPixel(x, y) counts rows from the bottom
            auto const px = [&](int x, int y) {
                uint8_t const* p = r.palette_pixels.data() + (size_t(palette.index()) * palette_width * palette_height
                    + size_t(palette_height - 1 - y) * palette_width + size_t(x)) * 3;
                return rw::vec3{float(p[0]) / 255.f, float(p[1]) / 255.f, float(p[2]) / 255.f};
            };
            pal.black = px(2, 7);
            pal.effect_a = px(31, 5);
            pal.effect_b = px(31, 3);
            return pal;
        }

        void build_rot(state& r, sys::game_state const& s, world::room_view const& room, float alpha, bool front) {
            auto const& w = s.world;
            painter p{r.batch};
            auto const mix = [](rw::vec3 a, rw::vec3 b, float t) { return a + (b - a) * mathf::clamp01(t); };
            rot_palette const pal = room_rot_palette(r, s, room.id);
            if (front) {
                // CorruptionDarkness: a blot of black goo
                w.room_for_each_rot_placed_location(room.id, [&](dcon::rot_placed_location_id l) {
                    auto const o = w.rot_placed_location_get_placed(l);
                    if (w.rot_placed_get_kind(o) == world::rot_kind::darkness)
                        p.glow(w.rot_placed_get_pos(o), rw::magnitude(w.rot_placed_get_handle(o)), to_rgba(pal.black, 0.85f), to_rgba(pal.black, 0.f));
                });
            }
            auto const sys = rot::system_of(s, room.id);
            if (!sys)
                return;
            bool const gw = w.rot_system_get_gw_mode(sys);
            rw::vec3 const sys_effect = w.rot_system_get_effect_color(sys);
            rw::vec3 const sys_eye = w.rot_system_get_eye_color(sys);
            // CustomRotData.RotColor
            auto const custom_color = [&](dcon::rot_placed_id c, rw::vec3 fallback) {
                if (!c)
                    return fallback;
                switch (w.rot_placed_get_custom(c).color) {
                case world::rot_color::effect_color_a: return pal.effect_a;
                case world::rot_color::effect_color_b: return pal.effect_b;
                default: return fallback;
                }
            };
            // flicker of the eyes (Random.value in RenderSlits)
            static uint32_t flicker = 0x1234567u;
            auto const flick = [&] {
                flicker = flicker * 1664525u + 1013904223u;
                return float(flicker >> 8) * (1.f / 16777215.f);
            };

            if (!front) {
                // TubeGraphic: a 4 pixel wide mesh along the graphic segments, then the bumps
                w.rot_system_for_each_rot_tube_membership(sys, [&](dcon::rot_tube_membership_id m) {
                    auto const t = w.rot_tube_membership_get_tube(m);
                    auto const g = w.rot_tube_get_graphic(t);
                    uint32_t const n = g.size();
                    if (n < 2)
                        return;
                    auto const kind = w.rot_tube_get_kind(t);
                    float const deadness = w.rot_tube_get_deadness(t);
                    auto const fac = [&](float f) {
                        switch (kind) {
                        case world::rot_tube_kind::leg: return mathf::pow(f, 1.5f) * 0.4f * (1.f - deadness);
                        case world::rot_tube_kind::tendril: return 0.f;
                        case world::rot_tube_kind::climb: return std::sin(f * mathf::pi) * 0.4f;
                        }
                        return 0.f;
                    };
                    rw::vec3 effect = sys_effect;
                    if (kind == world::rot_tube_kind::leg) {
                        auto const b = w.rot_tube_get_bulb(t);
                        effect = custom_color(w.rot_bulb_get_custom(b), sys_effect);
                    }
                    auto const at = [&](uint32_t i) { return rw::lerp(g[i].last_pos, g[i].pos, alpha); };
                    vec2 prev = at(0);
                    prev += rw::custom::dir_vec(at(1), prev) * 1.f;
                    uint32_t prev_col = to_rgba(mix(pal.black, effect, fac(0.f)));
                    for (uint32_t j = 0; j < n; ++j) {
                        vec2 const cur = at(j);
                        vec2 const a = rw::custom::perpendicular_vector(rw::normalized(prev - cur)) * 2.f;
                        uint32_t const col = to_rgba(mix(pal.black, effect, fac(float(j) / float(n - 1))));
                        r.batch.push_back(vertex{prev.x - a.x, prev.y - a.y, prev_col, 0.f, 0.f});
                        r.batch.push_back(vertex{prev.x + a.x, prev.y + a.y, prev_col, 0.f, 0.f});
                        r.batch.push_back(vertex{cur.x + a.x, cur.y + a.y, col, 0.f, 0.f});
                        r.batch.push_back(vertex{prev.x - a.x, prev.y - a.y, prev_col, 0.f, 0.f});
                        r.batch.push_back(vertex{cur.x + a.x, cur.y + a.y, col, 0.f, 0.f});
                        r.batch.push_back(vertex{cur.x - a.x, cur.y - a.y, col, 0.f, 0.f});
                        prev = cur;
                        prev_col = col;
                    }
                    // TubeGraphic.OnTubePos
                    float const step = 1.f / float(n - 1);
                    for (auto const& bump : w.rot_tube_get_bumps(t)) {
                        vec2 const p0 = rot::tube_graphic_pos(s, t, bump.pos.y - step, alpha);
                        vec2 const p1 = rot::tube_graphic_pos(s, t, bump.pos.y + step, alpha);
                        vec2 const c = rot::tube_graphic_pos(s, t, bump.pos.y, alpha) + rw::custom::perpendicular_vector(rw::custom::dir_vec(p0, p1)) * bump.pos.x;
                        float const rad = mathf::lerp(2.f, 6.f, bump.size);
                        p.circle(c, rad, to_rgba(mix(pal.black, effect, fac(bump.pos.y))));
                        if (bump.eye_size > 0.f)
                            p.circle(c, rad * bump.eye_size, to_rgba(gw ? pal.black : effect));
                    }
                });
            }

            w.rot_system_for_each_rot_bulb_membership(sys, [&](dcon::rot_bulb_membership_id m) {
                auto const b = w.rot_bulb_membership_get_bulb(m);
                bool const midground = w.rot_bulb_get_body_in_midground(b);
                vec2 const pos = rw::lerp(w.rot_bulb_get_last_pos(b), w.rot_bulb_get_pos(b), alpha);
                float const rad = w.rot_bulb_get_rad(b);
                vec2 const stalk = w.rot_bulb_get_eye_stalk_pos(b);
                auto const cp = w.rot_bulb_get_custom(b);
                world::rot_custom_data const cd = cp ? w.rot_placed_get_custom(cp) : world::rot_custom_data{};
                rw::vec3 const effect = custom_color(cp, sys_effect);
                float const rotation = std::atan2(pos.x - stalk.x, pos.y - stalk.y) / 6.2831855f * 360.f + w.rot_bulb_get_rotation(b);
                if (front && w.rot_bulb_get_has_black_goo(b)) {
                    // the "corruption" goo sprite under the bulb
                    float const goo = rad * mathf::clamp(rad * 0.35f, 3.f, 7.f) / 100.f * (cp ? cd.darkness_scale : 1.f);
                    p.glow(w.rot_bulb_get_stuck_pos(b), goo * 100.f, to_rgba(pal.black, 0.8f), to_rgba(pal.black, 0.f));
                }
                if (midground != front) {
                    // JaggedCircle: a ragged disc turned with the bulb
                    uint32_t const col = to_rgba(cp ? mix(pal.black, effect, w.rot_bulb_get_color_fac(b)) : pal.black);
                    constexpr int segs = 12;
                    uint32_t h = uint32_t(b.index()) * 2654435761u;
                    float radii[segs];
                    for (int k = 0; k < segs; ++k) {
                        h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                        radii[k] = rad * (0.82f + 0.18f * float(h & 0xFFFF) / 65535.f);
                    }
                    for (int k = 0; k < segs; ++k) {
                        float const a0 = (rotation + float(k) * 360.f / segs) * mathf::deg2rad;
                        float const a1 = (rotation + float(k + 1) * 360.f / segs) * mathf::deg2rad;
                        p.tri(pos, pos + vec2{std::sin(a0), std::cos(a0)} * radii[k], pos + vec2{std::sin(a1), std::cos(a1)} * radii[(k + 1) % segs], col);
                    }
                }
                if (!front)
                    return;
                if (w.rot_bulb_get_has_dot(b)) {
                    vec2 const at = pos + rw::custom::dir_vec(stalk, pos) * rad * (1.f - cd.max_eye_size);
                    p.circle(at, rad * w.rot_bulb_get_dot_scale(b), to_rgba(mix(pal.black, effect, mathf::lerp(cd.min_color, cd.max_color, w.rot_bulb_get_corruption_level(b)))));
                }
                if (!w.rot_bulb_get_has_eye(b))
                    return;
                // Bulb.RenderSlits: a cross of two slit meshes bulging towards the look direction
                rw::vec3 const eye = custom_color(cp, sys_eye);
                float const num = mathf::lerp(rad * 0.5f, rad, w.rot_bulb_get_eye_rad(b));
                float const num2 = mathf::pow(mathf::max(0.f, mathf::lerp(w.rot_bulb_get_last_closed(b), w.rot_bulb_get_closed(b), alpha)), 0.6f);
                float const num3 = 1.f - num2;
                vec2 const look = rw::lerp(w.rot_bulb_get_last_look_dir(b), w.rot_bulb_get_look_dir(b), alpha);
                vec2 const out = rw::custom::dir_vec(stalk, pos);
                float num4 = mathf::lerp(w.rot_bulb_get_last_focus(b), w.rot_bulb_get_focus(b), alpha)
                    * mathf::pow(mathf::inverse_lerp(-1.f, 1.f, rw::dot(out, rw::normalized(w.rot_bulb_get_look_dir(b)))), 0.7f);
                num4 = mathf::max(num4, num2);
                float num5 = mathf::inverse_lerp(0.f, 50.f, rw::distance(stalk, pos + out * rad)) * 0.9f;
                num5 = mathf::lerp(num5, 1.f, 0.5f * num4);
                vec2 const bulge_dir = rw::lerp(out * num5, look, rw::magnitude(look) * 0.5f);
                auto const bulge = [&](vec2 v) { return rw::lerp(v, rw::clamp_magnitude(v + bulge_dir * rad, rad), rw::magnitude(bulge_dir)); };
                uint32_t const col = to_rgba(mix(eye, rw::vec3{1.f, 1.f, 1.f}, mathf::lerp(flick() * w.rot_bulb_get_light(b), 1.f, num2)));
                for (int i = 0; i < 2; ++i) {
                    vec2 const v2 = rw::custom::deg_to_vec(rotation + 90.f * float(i));
                    vec2 const a = rw::custom::perpendicular_vector(v2);
                    vec2 verts[10];
                    float const squint = mathf::lerp(1.f, 0.6f, num4);
                    verts[0] = pos + bulge(v2 * num * 0.9f * squint);
                    verts[9] = pos + bulge(v2 * -num * 0.9f * squint);
                    for (int j = 1; j < 5; ++j) {
                        for (int k = 0; k < 2; ++k) {
                            float const d = num * (j < 3 ? 0.7f : 0.25f) * (k == 0 ? 1.f : -1.f) * squint;
                            int const idx = k == 0 ? j : 9 - j;
                            float const d2 = num3 * (j < 3 ? 0.5f : 1.f) * (idx % 2 == 0 ? 1.f : -1.f) * mathf::lerp(1.f, 2.5f, num4);
                            verts[idx] = pos + bulge(v2 * d + a * d2);
                        }
                    }
                    for (int k = 0; k < 8; ++k)
                        p.tri(verts[k], verts[k + 1], verts[k + 2], col);
                }
            });
        }

        // RippleRing: thin violet rings spreading out
        void build_ripple_rings(state& r, sys::game_state const& s, world::room_view const& room) {
            painter p{r.batch};
            for (auto const& ring : s.ripple_rings) {
                if (ring.room != room.id)
                    continue;
                float const e = 1.f - (1.f - ring.life) * (1.f - ring.life);
                float const rad = ring.rad * e;
                float const width = 1.f + 3.f * (1.f - ring.life);
                uint32_t const col = to_rgba(rw::vec3{0.62f, 0.56f, 1.f}, mathf::clamp01(ring.intensity) * (1.f - ring.life) * 0.9f);
                constexpr int segs = 48;
                for (int k = 0; k < segs; ++k) {
                    float const a0 = float(k) / segs * 6.2831853f, a1 = float(k + 1) / segs * 6.2831853f;
                    vec2 const d0{std::cos(a0), std::sin(a0)}, d1{std::cos(a1), std::sin(a1)};
                    vec2 const i0 = ring.pos + d0 * rad, o0 = ring.pos + d0 * (rad + width);
                    vec2 const i1 = ring.pos + d1 * rad, o1 = ring.pos + d1 * (rad + width);
                    p.tri(i0, o0, o1, col);
                    p.tri(i0, o1, i1, col);
                }
            }
        }

        // copies the frame drawn so far into scene_texture (Unity's GrabPass)
        void grab_scene(state& r) {
            if (r.scene_texture == 0)
                glGenTextures(1, &r.scene_texture);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, r.scene_texture);
            if (r.scene_width != r.width || r.scene_height != r.height) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, r.width, r.height, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                r.scene_width = r.width;
                r.scene_height = r.height;
            }
            glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, r.width, r.height);
        }

        // the far side palette of a warp: ripple warps, limited use (bad) warps, the Outer Rim's
        float warp_variant(sys::game_state const& s, dcon::warp_placed_id placed, dcon::room_id room) {
            auto const& w = s.world;
            if (w.warp_placed_get_ripple_warp(placed))
                return 1.f;
            auto const region = w.room_get_region_from_region_membership(room);
            if (region && s.to_string_view(w.region_get_name(region)) == "wora")
                return 3.f;
            if (w.warp_placed_get_limited_use(placed))
                return 2.f;
            return 0.f;
        }

        // WarpTear for every warp point of the room
        void draw_warp_tears(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, glm::mat4 const& proj, float seconds) {
            auto const& w = s.world;
            if (r.warp_tear_program == 0)
                return;
            bool grabbed = false;
            w.room_for_each_warp_point_location(room.id, [&](dcon::warp_point_location_id l) {
                auto const wp = w.warp_point_location_get_point(l);
                dcon::warp_placed_id const placed = w.warp_point_get_placed(wp);
                float const fade = mathf::lerp(w.warp_point_get_last_fade(wp), w.warp_point_get_fade(wp), alpha);
                if (fade <= 0.f)
                    return;
                vec2 const c = w.warp_placed_get_pos(placed);
                float const rad = warps::tear_radius;
                // off screen: nothing to bend
                float const view_w = float(r.width) / cam.zoom, view_h = float(r.height) / cam.zoom;
                if (c.x + rad * 0.5f < cam.pos.x || c.x - rad * 0.5f > cam.pos.x + view_w || c.y + rad * 0.5f < cam.pos.y || c.y - rad * 0.5f > cam.pos.y + view_h)
                    return;
                if (!grabbed) {
                    grab_scene(r);
                    grabbed = true;
                }
                float const open = mathf::lerp(w.warp_point_get_last_open(wp), w.warp_point_get_open(wp), alpha);
                r.batch.clear();
                vertex const bl{c.x - rad, c.y - rad, 0xFFFFFFFFu, -1.f, -1.f};
                vertex const br{c.x + rad, c.y - rad, 0xFFFFFFFFu, 1.f, -1.f};
                vertex const tr{c.x + rad, c.y + rad, 0xFFFFFFFFu, 1.f, 1.f};
                vertex const tl{c.x - rad, c.y + rad, 0xFFFFFFFFu, -1.f, 1.f};
                r.batch.insert(r.batch.end(), {bl, br, tr, bl, tr, tl});
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
                GLuint const prog = r.warp_tear_program;
                glUseProgram(prog);
                auto const loc = [&](char const* n) { return glGetUniformLocation(prog, n); };
                glUniformMatrix4fv(loc("u_proj"), 1, GL_FALSE, glm::value_ptr(proj));
                glUniform2f(loc("u_screen"), float(r.width), float(r.height));
                glUniform1f(loc("u_radius_px"), rad * cam.zoom);
                glUniform1f(loc("u_open"), open);
                glUniform1f(loc("u_fade"), fade);
                glUniform1f(loc("u_time"), seconds);
                glUniform1f(loc("u_seed"), float(placed.index() % 97) * 0.173f);
                glUniform1f(loc("u_variant"), warp_variant(s, placed, room.id));
                glUniform1f(loc("u_ghost"), warps::other_layer(s, wp) ? 1.f : 0.f);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, r.scene_texture);
                glDrawArrays(GL_TRIANGLES, 0, 6);
            });
        }

        // RoomCamera.warpPointTimer's screen effect: winding into the warp,
        // then out of the destination's
        void draw_warp_screen(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, glm::mat4 const& proj, float seconds) {
            auto const& seq = s.warp;
            if (!seq.active || r.warp_screen_program == 0)
                return;
            bool const second_half = seq.transferred;
            if ((second_half ? seq.to_room : seq.from_room) != room.id)
                return;
            float const progress = mathf::lerp(seq.last_progress, seq.progress, alpha) / seq.duration;
            vec2 const at = second_half ? seq.to_pos : seq.from_pos;
            grab_scene(r);
            float const view_w = float(r.width) / cam.zoom, view_h = float(r.height) / cam.zoom;
            r.batch.clear();
            vertex const bl{cam.pos.x, cam.pos.y, 0xFFFFFFFFu, 0.f, 0.f};
            vertex const br{cam.pos.x + view_w, cam.pos.y, 0xFFFFFFFFu, 1.f, 0.f};
            vertex const tr{cam.pos.x + view_w, cam.pos.y + view_h, 0xFFFFFFFFu, 1.f, 1.f};
            vertex const tl{cam.pos.x, cam.pos.y + view_h, 0xFFFFFFFFu, 0.f, 1.f};
            r.batch.insert(r.batch.end(), {bl, br, tr, bl, tr, tl});
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            GLuint const prog = r.warp_screen_program;
            glUseProgram(prog);
            auto const loc = [&](char const* n) { return glGetUniformLocation(prog, n); };
            glUniformMatrix4fv(loc("u_proj"), 1, GL_FALSE, glm::value_ptr(proj));
            glUniform2f(loc("u_screen"), float(r.width), float(r.height));
            glUniform2f(loc("u_center"), (at.x - cam.pos.x) * cam.zoom, (at.y - cam.pos.y) * cam.zoom);
            glUniform1f(loc("u_progress"), progress);
            glUniform1f(loc("u_time"), seconds);
            glUniform1f(loc("u_variant"), seq.ripple ? 1.f : 0.f);
            auto const knob = [&](char const* n, uint8_t v) { glUniform1f(loc(n), float(v) / 15.f); };
            knob("u_vignette", seq.effect.vignette);
            knob("u_darkness", seq.effect.darkness);
            knob("u_spiral_gap", seq.effect.spiral_gap);
            knob("u_swirl", seq.effect.swirl);
            knob("u_lensing", seq.effect.lensing);
            knob("u_noise", seq.effect.noise);
            knob("u_twist", seq.effect.twist);
            knob("u_agitation", seq.effect.agitation);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, r.scene_texture);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        // ring of `n` arc pieces from angle a0 to a1 (radians, clockwise from the top in screen space)
        void arc(painter& p, vec2 c, float rad, float width, float a0, float a1, uint32_t col) {
            int const segs = std::max(2, int(std::fabs(a1 - a0) * 12.f));
            for (int k = 0; k < segs; ++k) {
                float const t0 = a0 + (a1 - a0) * float(k) / float(segs);
                float const t1 = a0 + (a1 - a0) * float(k + 1) / float(segs);
                vec2 const d0{std::sin(t0), -std::cos(t0)}, d1{std::sin(t1), -std::cos(t1)};
                p.tri(c + d0 * rad, c + d0 * (rad + width), c + d1 * (rad + width), col);
                p.tri(c + d0 * rad, c + d1 * (rad + width), c + d1 * rad, col);
            }
        }

        // The karma meter (any slugcat) or the ripple meter (the Watcher) in
        // the bottom left corner, screen space. Plain shapes: karma is a ring
        // of ten notches lit up to the karma level, the cap's notches faint;
        // the ripple level is a set of nested rings, half levels as half
        // rings, with the camouflage charge left as an arc around them
        void draw_meters(state& r, sys::game_state const& s) {
            auto const& w = s.world;
            dcon::player_id p0;
            w.for_each_player([&](dcon::player_id p) {
                if (!p0)
                    p0 = p;
            });
            if (!p0)
                return;
            r.batch.clear();
            painter p{r.batch};
            vec2 const c{58.f, float(r.height) - 58.f};
            p.glow(c, 46.f, rgba(0, 0, 0, 150), rgba(0, 0, 0, 0));
            if (warps::is_watcher(s, p0)) {
                float const level = s.ripple_level;
                bool const layer = warps::active_ripple_layer(s) == 1;
                uint32_t const lit = layer ? rgba(255, 214, 120) : rgba(170, 156, 255);
                uint32_t const dim = rgba(90, 84, 130, 110);
                for (int i = 0; i < 5; ++i) {
                    float const rad = 7.f + float(i) * 5.f;
                    float const fill = mathf::clamp01(level - float(i));
                    arc(p, c, rad, 2.f, 0.f, 6.2831853f, dim);
                    if (fill > 0.f)
                        arc(p, c, rad, 2.f, 0.f, 6.2831853f * fill, lit);
                }
                p.circle(c, 3.f, level > 0.f ? lit : dim);
                // camouflage charge still available, and a glow while camouflaged
                float const left = 1.f - w.player_get_camo_charge(p0) / (level >= 5.f ? 4800.f : level >= 4.f ? 3600.f : level >= 2.f ? 2400.f : 1600.f);
                arc(p, c, 36.f, 3.f, 0.f, 6.2831853f * mathf::clamp01(left), rgba(220, 220, 240, 170));
                float const camo = w.player_get_camo_progress(p0);
                if (camo > 0.f)
                    p.glow(c, 44.f, to_rgba(rw::vec3{0.65f, 0.6f, 1.f}, 0.35f * camo), to_rgba(rw::vec3{0.65f, 0.6f, 1.f}, 0.f));
            } else {
                for (int i = 0; i < 10; ++i) {
                    float const a = float(i) / 10.f * 6.2831853f;
                    vec2 const d{std::sin(a), -std::cos(a)};
                    uint32_t const col = i <= s.karma ? rgba(240, 236, 220) : i <= s.karma_cap ? rgba(140, 136, 128, 170) : rgba(70, 68, 64, 90);
                    float const len = i <= s.karma ? 12.f : 8.f;
                    p.line(c + d * 18.f, c + d * (18.f + len), 3.f, col);
                }
                arc(p, c, 14.f, 2.f, 0.f, 6.2831853f, rgba(240, 236, 220, 200));
                // the level itself: as many dots as karma above the first
                for (int i = 0; i < s.karma; ++i) {
                    float const a = float(i) / float(std::max(1, s.karma)) * 6.2831853f;
                    p.circle(c + vec2{std::sin(a), -std::cos(a)} * 7.f, 1.6f, rgba(240, 236, 220));
                }
                p.circle(c, 2.5f, rgba(240, 236, 220));
            }
            glm::mat4 const proj = glm::ortho(0.f, float(r.width), float(r.height), 0.f);
            glUseProgram(r.program);
            glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
            glUniform1i(r.u_textured, 0);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLES, 0, GLsizei(r.batch.size()));
        }

        // RoomPalette.GetPixel(x, y) of the room's palette (rows count from the bottom)
        rw::vec3 palette_texel(state const& r, sys::game_state const& s, dcon::room_id room, int x, int y) {
            auto const palette = s.world.room_get_palette(room).id;
            if (!palette || r.palette_pixels.empty())
                return rw::vec3{0.f, 0.f, 0.f};
            uint8_t const* p = r.palette_pixels.data() + (size_t(palette.index()) * palette_width * palette_height
                + size_t(palette_height - 1 - y) * palette_width + size_t(x)) * 3;
            return rw::vec3{float(p[0]) / 255.f, float(p[1]) / 255.f, float(p[2]) / 255.f};
        }

        uint32_t decal_texture(state& r, sys::game_state const& s, std::string const& name) {
            auto it = r.decal_textures.find(name);
            if (it != r.decal_textures.end())
                return it->second.texture;
            state::decal_texture t;
            image im;
            if (load_image(s.fs, s.fs.get_root().get_single_subdir("decals").get_single_file(name + ".png"), 4, im)) {
                t.texture = make_texture_2d(im, GL_RGBA, GL_RGBA8);
                glBindTexture(GL_TEXTURE_2D, t.texture);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            } else {
                reports::add_log("missing decal \"" + name + "\"");
            }
            r.decal_textures.emplace(name, t);
            return t.texture;
        }

        // CustomDecal: the image stretched over the quad, split into a grid
        // (finer for bigger decals) whose inner points the decal's noise
        // jitters, its corners' opacity blended across it, drawn once per
        // camera image under it
        void draw_decals(state& r, sys::game_state const& s, world::room_view const& room, glm::mat4 const& proj, int pass, std::vector<vec2> const& cams) {
            auto const& w = s.world;
            if (r.decal_program == 0 || cams.empty())
                return;
            auto const palette = w.room_get_palette(room.id).id;
            GLuint const prog = r.decal_program;
            auto const loc = [&](char const* n) { return glGetUniformLocation(prog, n); };
            bool set_up = false;
            w.room_for_each_decal_location(room.id, [&](dcon::decal_location_id l) {
                auto const d = w.decal_location_get_decal(l);
                uint32_t const tex = decal_texture(r, s, std::string{s.to_string_view(w.decal_placed_get_image(d))});
                if (tex == 0)
                    return;
                vec2 const q[4] = {w.decal_placed_get_pos(d), w.decal_placed_get_pos(d) + w.decal_placed_get_handle_a(d),
                    w.decal_placed_get_pos(d) + w.decal_placed_get_handle_b(d), w.decal_placed_get_pos(d) + w.decal_placed_get_handle_c(d)};
                float longest = 0.f;
                for (int k = 0; k < 4; ++k)
                    longest = std::max(longest, rw::distance(q[k], q[(k + 1) % 4]));
                int const div = std::clamp(int(std::lround(longest / 150.f)), 1, 20);
                float const noise = w.decal_placed_get_noise(d);
                // CustomDecalData.vertices: per corner (opacity, smudge)
                vec2 const cvv[4] = {w.decal_placed_get_uv_a(d), w.decal_placed_get_uv_b(d), w.decal_placed_get_uv_c(d), w.decal_placed_get_uv_d(d)};
                float const cv[4][2] = {{cvv[0].x, cvv[0].y}, {cvv[1].x, cvv[1].y}, {cvv[2].x, cvv[2].y}, {cvv[3].x, cvv[3].y}};
                uint32_t h = uint32_t(int32_t(q[0].x + q[0].y + q[1].x + q[1].y + q[2].x + q[2].y + q[3].x + q[3].y)) * 2654435761u + 1u;
                auto const roll = [&] {
                    h ^= h << 13; h ^= h >> 17; h ^= h << 5;
                    return float(h & 0xFFFFFF) / 16777215.f;
                };
                std::vector<vertex> grid(size_t((div + 1) * (div + 1)));
                for (int i = 0; i <= div; ++i) {
                    float const ti = float(i) / float(div);
                    for (int j = 0; j <= div; ++j) {
                        float const tj = float(j) / float(div);
                        // j runs along q0 -> q1, i along q0 -> q3
                        vec2 const pos = rw::lerp(rw::lerp(q[0], q[3], ti), rw::lerp(q[1], q[2], ti), tj);
                        // q0 bottom left, q1 top left, q2 top right, q3 bottom right
                        float u = ti;
                        float v = tj;
                        if (noise > 0.f && i > 0 && j > 0 && i < div && j < div) {
                            // the decal's noise roughens the image's stretch inside its edges
                            u = mathf::clamp01(u + (roll() - 0.5f) * noise * 0.5f / float(div));
                            v = mathf::clamp01(v + (roll() - 0.5f) * noise * 0.5f / float(div));
                        }
                        // the corners' opacity (and smudge), blended across the
                        // quad and broken up by the noise
                        auto const corners = [&](int k) {
                            return mathf::lerp(mathf::lerp(cv[3][k], cv[2][k], ti), mathf::lerp(cv[0][k], cv[1][k], ti), tj);
                        };
                        float opacity = mathf::clamp01(corners(0));
                        float const smudge = mathf::clamp01(corners(1));
                        if (noise > 0.f) {
                            opacity = std::pow(opacity, 1.f + (roll() - 0.5f) * noise);
                            opacity = mathf::lerp(opacity, roll(), noise * std::pow(1.f - 2.f * std::fabs(smudge - 0.5f), 2.5f));
                        }
                        grid[size_t(j * (div + 1) + i)] = vertex{pos.x, pos.y, rgba(uint8_t(smudge * 255.f), 255, 255, uint8_t(mathf::clamp01(opacity) * 255.f + 0.5f)), u, v};
                    }
                }
                r.batch.clear();
                for (int j = 0; j < div; ++j)
                    for (int i = 0; i < div; ++i) {
                        auto const& a = grid[size_t(j * (div + 1) + i)];
                        auto const& b = grid[size_t(j * (div + 1) + i + 1)];
                        auto const& c = grid[size_t((j + 1) * (div + 1) + i + 1)];
                        auto const& e = grid[size_t((j + 1) * (div + 1) + i)];
                        r.batch.insert(r.batch.end(), {a, b, c, a, c, e});
                    }
                // bounds, to skip cameras it doesn't touch
                vec2 lo = q[0], hi = q[0];
                for (auto const& p : q) {
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
                if (!set_up) {
                    set_up = true;
                    glUseProgram(prog);
                    glUniformMatrix4fv(loc("u_proj"), 1, GL_FALSE, glm::value_ptr(proj));
                    glUniform1i(loc("u_index"), palette ? palette.index() : 0);
                    glUniform1i(loc("u_pass"), pass);
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, r.room_texture);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D_ARRAY, r.palette_texarray);
                }
                glUniform1f(loc("u_from"), w.decal_placed_get_from_depth(d));
                glUniform1f(loc("u_to"), w.decal_placed_get_to_depth(d));
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, tex);
                glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
                for (size_t c = 0; c < cams.size(); ++c) {
                    vec2 const o = cams[c];
                    if (hi.x < o.x || lo.x > o.x + float(camera_width) || hi.y < o.y || lo.y > o.y + float(camera_height))
                        continue;
                    glUniform1i(loc("u_camera_index"), GLint(c));
                    glUniform2f(loc("u_cam_origin"), o.x, o.y);
                    glDrawArrays(GL_TRIANGLES, 0, GLsizei(r.batch.size()));
                }
            });
            glActiveTexture(GL_TEXTURE0);
        }

        // particles.hpp's effects, plain shapes
        void build_particles(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            painter p{r.batch};
            for (auto const& pt : s.particles) {
                if (pt.room != room.id)
                    continue;
                vec2 const pos = rw::lerp(pt.last_pos, pt.pos, alpha);
                float const fade = 1.f - pt.life;
                switch (particles::kind(pt.kind)) {
                case particles::kind::drop: {
                    vec2 const tail = pos - (pt.pos - pt.last_pos) * 0.8f;
                    if (rw::distance(tail, pos) < 0.5f)
                        p.circle(pos, 0.7f * pt.size, to_rgba(pt.color, pt.alpha));
                    else
                        p.line(tail, pos, 1.1f * pt.size, to_rgba(pt.color, pt.alpha));
                    break;
                }
                case particles::kind::rain:
                    p.line(pos - pt.vel * 1.4f, pos, 1.f * pt.size, to_rgba(pt.color, pt.alpha));
                    break;
                case particles::kind::spark:
                    p.line(rw::lerp(pt.last_pos, pos, 0.2f) - (pt.pos - pt.last_pos) * 0.6f, pos, pt.size, to_rgba(pt.color, fade));
                    break;
                case particles::kind::dust:
                    p.glow(pos, pt.size, to_rgba(pt.color, pt.alpha * fade), to_rgba(pt.color, 0.f));
                    break;
                case particles::kind::bubble: {
                    // a thin ring with a glint
                    constexpr int segs = 10;
                    uint32_t const col = to_rgba(pt.color, pt.alpha * mathf::min(1.f, fade * 3.f));
                    for (int k = 0; k < segs; ++k) {
                        float const a0 = float(k) / segs * 6.2831853f, a1 = float(k + 1) / segs * 6.2831853f;
                        vec2 const d0{std::cos(a0), std::sin(a0)}, d1{std::cos(a1), std::sin(a1)};
                        p.tri(pos + d0 * pt.size, pos + d0 * (pt.size + 0.8f), pos + d1 * (pt.size + 0.8f), col);
                        p.tri(pos + d0 * pt.size, pos + d1 * (pt.size + 0.8f), pos + d1 * pt.size, col);
                    }
                    p.circle(pos + vec2{-0.35f, 0.35f} * pt.size, 0.45f, to_rgba(rw::vec3{1.f, 1.f, 1.f}, 0.8f * fade));
                    break;
                }
                case particles::kind::flash:
                    p.glow(pos, pt.size, to_rgba(pt.color, pt.alpha * fade), to_rgba(pt.color, 0.f));
                    break;
                }
            }
        }

        // the water: its body (the scene behind refracted and tinted, see
        // water.frag) from the front edge down, then the top of the surface
        // as a band of rows going back, shaded by their slope
        void draw_water(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, glm::mat4 const& proj, float seconds) {
            if (!room.water)
                return;
            auto const* surf = water::surface(s, room.id);
            float const view_w = float(r.width) / cam.zoom, view_h = float(r.height) / cam.zoom;
            float const x0 = cam.pos.x - 20.f, x1 = cam.pos.x + view_w + 20.f;
            float const bottom = cam.pos.y - 40.f;
            if (surf == nullptr || r.water_program == 0) {
                r.batch.clear();
                painter p{r.batch};
                p.rect(-200.f, -200.f, room.pixel_width() + 400.f, room.float_water_level + 200.f, col_water);
                glUseProgram(r.program);
                glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
                glUniform1i(r.u_textured, 0);
                upload_and_draw(r, 0, r.batch.size());
                return;
            }
            float const level = surf->level;
            // only a surface below the view leaves nothing to draw; one above
            // it still has the view under water
            if (level + 40.f < cam.pos.y)
                return;
            float const top_clip = cam.pos.y + view_h + 40.f;
            rw::vec3 const shallow = palette_texel(r, s, room.id, 4, 7);
            rw::vec3 const deep = palette_texel(r, s, room.id, 5, 7);
            rw::vec3 const top1 = palette_texel(r, s, room.id, 6, 7);
            rw::vec3 const top2 = palette_texel(r, s, room.id, 7, 7);
            rw::vec3 const shine = palette_texel(r, s, room.id, 8, 7);
            constexpr float step = 4.f;
            int const n = int((x1 - x0) / step) + 1;
            auto const hgt = [&](float x, float z) { return water::height(s, *surf, x, z, alpha); };

            // the body
            grab_scene(r);
            r.batch.clear();
            for (int i = 0; i + 1 < n; ++i) {
                float const xa = x0 + float(i) * step, xb = xa + step;
                float const ya = level + hgt(xa, 0.f), yb = level + hgt(xb, 0.f);
                // clipped to the view, the depth still measured from the surface
                float const ca = mathf::min(ya, top_clip), cb = mathf::min(yb, top_clip);
                vertex const ta{xa, ca, 0xFFFFFFFFu, xa, ya - ca}, tb{xb, cb, 0xFFFFFFFFu, xb, yb - cb};
                vertex const ba{xa, bottom, 0xFFFFFFFFu, xa, ya - bottom}, bb{xb, bottom, 0xFFFFFFFFu, xb, yb - bottom};
                r.batch.insert(r.batch.end(), {ta, tb, bb, ta, bb, ba});
            }
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            GLuint const prog = r.water_program;
            glUseProgram(prog);
            auto const loc = [&](char const* nm) { return glGetUniformLocation(prog, nm); };
            glUniformMatrix4fv(loc("u_proj"), 1, GL_FALSE, glm::value_ptr(proj));
            glUniform2f(loc("u_screen"), float(r.width), float(r.height));
            glUniform1f(loc("u_time"), seconds);
            glUniform1f(loc("u_zoom"), cam.zoom);
            glUniform3f(loc("u_shallow"), shallow.x, shallow.y, shallow.z);
            glUniform3f(loc("u_deep"), deep.x, deep.y, deep.z);
            glUniform3f(loc("u_shine"), shine.x, shine.y, shine.z);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, r.scene_texture);
            upload_and_draw(r, 0, r.batch.size());

            if (level - 60.f > top_clip)
                return;
            // the surface band: lit from above and a little behind, the slopes
            // facing the light catching the shine
            r.batch.clear();
            rw::vec3 const light = glm::normalize(rw::vec3{-0.35f, 0.85f, -0.4f});
            rw::vec3 const eye = glm::normalize(rw::vec3{0.f, 0.55f, -0.85f});
            rw::vec3 const half = glm::normalize(light + eye);
            auto const shade = [&](float x, float z) {
                float const dx = (hgt(x + 3.f, z) - hgt(x - 3.f, z)) / 6.f;
                float const z0 = mathf::max(0.f, z - 0.5f), z1 = mathf::min(float(water::rows - 1), z + 0.5f);
                float const dz = (hgt(x, z1) - hgt(x, z0)) / ((z1 - z0) * 40.f);
                rw::vec3 const nrm = glm::normalize(rw::vec3{-dx, 1.f, -dz});
                float const diffuse = mathf::max(0.f, glm::dot(nrm, light));
                float const spec = std::pow(mathf::max(0.f, glm::dot(nrm, half)), 48.f);
                float const t = z / float(water::rows - 1);
                rw::vec3 const base = top1 + (top2 - top1) * t;
                rw::vec3 const c = base * (0.55f + 0.45f * diffuse) + shine * spec * 1.4f;
                // see-through enough for whatever swims at the surface
                return to_rgba(c, mathf::lerp(0.62f, 0.4f, t) + spec * 0.3f);
            };
            auto const ypos = [&](float x, float z) { return level + hgt(x, z) + z / float(water::rows - 1) * water::depth_px; };
            for (uint32_t zr = 0; zr + 1 < water::rows; ++zr) {
                float const za = float(zr), zb = float(zr + 1);
                for (int i = 0; i + 1 < n; ++i) {
                    float const xa = x0 + float(i) * step, xb = xa + step;
                    vertex const a{xa, ypos(xa, za), shade(xa, za), 0.f, 0.f};
                    vertex const b{xb, ypos(xb, za), shade(xb, za), 0.f, 0.f};
                    vertex const c{xb, ypos(xb, zb), shade(xb, zb), 0.f, 0.f};
                    vertex const d{xa, ypos(xa, zb), shade(xa, zb), 0.f, 0.f};
                    r.batch.insert(r.batch.end(), {a, b, c, a, c, d});
                }
            }
            // the bright lip where the front of the surface meets the body
            painter p{r.batch};
            for (int i = 0; i + 1 < n; ++i) {
                float const xa = x0 + float(i) * step, xb = xa + step;
                p.line(vec2{xa, ypos(xa, 0.f)}, vec2{xb, ypos(xb, 0.f)}, 1.5f, to_rgba(shine, 0.55f));
            }
            glUseProgram(r.program);
            glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
            glUniform1i(r.u_textured, 0);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            upload_and_draw(r, 0, r.batch.size());
        }

        // RegionGateGraphics, minimal: each door is a slab sliding down from
        // above its opening while closing
        void build_gate_doors(state& r, sys::game_state const& s, world::room_view const& room) {
            auto const g = gates::gate_of(s, room.id);
            if (!g)
                return;
            painter p{r.batch};
            rw::vec3 const closed = s.world.gate_get_door_closed(g);
            float const bottom = float(gates::door_bottom_y) * 20.f;
            float const top = float(gates::door_top_y + 1) * 20.f;
            for (int i = 0; i < 3; ++i) {
                float const x = float(gates::door_first_x + i * gates::door_spacing) * 20.f;
                float const h = (top - bottom) * closed[i];
                if (h <= 0.f)
                    continue;
                p.rect(x, top - h, 40.f, h, col_gate_door);
                p.rect(x + 4.f, top - h, 32.f, 4.f, col_gate_door_edge);
                p.rect(x + 18.f, top - h, 4.f, h, col_gate_door_edge);
            }
        }

        // The vessel of a player travelling a shortcut: a soft circle gliding
        // along the pipe (vanilla blinks the pipe tile by tile), lighting the
        // pipe tiles around it
        void build_shortcut_glow(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            auto const& world = s.world;
            painter p{r.batch};
            world.for_each_player([&](dcon::player_id pid) {
                if (!world.player_get_in_shortcut(pid) || world.physical_object_get_room_from_object_location(world.player_get_body_from_player_body(pid)) != room.id)
                    return;
                rw::vec3 const col = world.player_get_stats(pid).body_color;
                auto const shade = [&](float a) { return to_rgba(rw::vec3{0.5f, 0.5f, 0.5f} + col * 0.5f, a); };
                vec2 const pos = shortcuts::vessel_position(s, pid, alpha);
                auto const path = world.shortcut_get_path(world.player_get_vessel_shortcut(pid));
                for (uint32_t i = 0; i < path.size(); ++i) {
                    vec2 const m = world::room_view::middle_of_tile(path[i]);
                    float const d = rw::distance(m, pos);
                    if (d > 70.f)
                        continue;
                    float const a = 0.55f * (1.f - d / 70.f);
                    p.rect(m.x - 5.f, m.y - 5.f, 10.f, 10.f, shade(a));
                }
                p.glow(pos, 26.f, shade(0.45f), shade(0.f));
                p.glow(pos, 8.f, shade(1.f), shade(0.2f));
            });
        }
    }

    void draw_frame(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, std::string const& hud) {
        auto const& world = s.world;
        SDL_GetWindowSizeInPixels(r.window, &r.width, &r.height);
        glViewport(0, 0, r.width, r.height);
        float const view_w = float(r.width) / cam.zoom;
        float const view_h = float(r.height) / cam.zoom;
        glm::mat4 const proj = glm::ortho(cam.pos.x, cam.pos.x + view_w, cam.pos.y, cam.pos.y + view_h);

        auto const palette = world.room_get_palette(room.id).id;
        bool const textured_room = r.room_program != 0 && r.room_texture != 0 && r.room_texture_layers > 0 && room.id == r.room_texture_room;

        // sky: the palette's first texel, like the vanilla level shader's sky layer
        rw::vec3 sky{18.f / 255.f, 18.f / 255.f, 24.f / 255.f};
        if (textured_room && palette && !r.palette_pixels.empty()) {
            uint8_t const* px = r.palette_pixels.data() + size_t(palette.index()) * palette_width * palette_height * 3;
            sky = rw::vec3{float(px[0]) / 255.f, float(px[1]) / 255.f, float(px[2]) / 255.f};
        }
        glClearColor(sky.x, sky.y, sky.z, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        r.batch.clear();
        glBindVertexArray(r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, r.vbo);

        // room camera images with the level shader. Pass 0 draws everything
        // behind the creature layers, pass 1 only the foreground depths
        // (vanilla puts the level's front layers over sprites)
        std::vector<vec2> cams;
        if (textured_room) {
            world.room_for_each_camera_location(room.id, [&](dcon::camera_location_id cl) {
                cams.push_back(world.room_camera_get_position(world.camera_location_get_camera(cl)));
            });
        }
        auto const draw_cameras = [&](int pass) {
            r.batch.clear();
            for (auto const c : cams) {
                // image top row is texture row 0
                vertex const bl{c.x, c.y, 0xFFFFFFFFu, 0.f, 1.f};
                vertex const br{c.x + float(camera_width), c.y, 0xFFFFFFFFu, 1.f, 1.f};
                vertex const tr{c.x + float(camera_width), c.y + float(camera_height), 0xFFFFFFFFu, 1.f, 0.f};
                vertex const tl{c.x, c.y + float(camera_height), 0xFFFFFFFFu, 0.f, 0.f};
                r.batch.insert(r.batch.end(), {bl, br, tr, bl, tr, tl});
            }
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            auto const& u = r.room_uniforms;
            glUseProgram(r.room_program);
            glUniformMatrix4fv(u.proj, 1, GL_FALSE, glm::value_ptr(proj));
            glUniform1i(u.pass, pass);
            glUniform1i(u.palette, palette ? palette.index() : 0);
            int16_t const fx_a = world.room_get_effect_color_a(room.id);
            int16_t const fx_b = world.room_get_effect_color_b(room.id);
            glUniform2i(u.effect_colors, fx_a < 0 ? 0 : fx_a, fx_b < 0 ? 0 : fx_b);
            glUniform1f(u.grime, world.room_get_grime(room.id));
            glUniform1f(u.night_time, r.night_time);
            glUniform1f(u.fog_factor, 0.5f);
            glUniform1f(u.rooftop_blend, 0.f);
            // the grime noise drifts with this clock: keep it slow, and wrap on
            // whole sine periods so it never jumps
            constexpr double grime_speed = 0.004; // radians per second
            glUniform1f(u.timer, float(std::fmod(double(SDL_GetTicks()) / 1000.0 * grime_speed, 6.283185307179586)));
            float const zeros[max_lights * 4] = {};
            glUniform4fv(u.lighting_data, max_lights, zeros);
            glUniform2fv(u.lighting_pos, max_lights, zeros);
            glUniform4f(u.replace_a, 0.f, 0.f, 0.f, 0.f);
            glUniform4f(u.replace_b, 0.f, 0.f, 0.f, 0.f);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D_ARRAY, r.room_texture);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D_ARRAY, r.palette_texarray);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, r.effects_texture);
            for (size_t i = 0; i < cams.size(); ++i) {
                glUniform1i(u.camera_index, GLint(i));
                upload_and_draw(r, i * 6, 6);
            }
            glActiveTexture(GL_TEXTURE0);
        };
        // untextured triangles, flat shader
        auto const draw_flat = [&](auto&& build) {
            r.batch.clear();
            build();
            if (r.batch.empty())
                return;
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
            glUseProgram(r.program);
            glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
            glUniform1i(r.u_textured, 0);
            upload_and_draw(r, 0, r.batch.size());
        };
        auto const draw_geometry = [&] {
            draw_flat([&] { build_geometry(r, s, room, cam, view_w, view_h, textured_room ? 110 : 255); });
        };

        float const seconds = float(std::fmod(double(SDL_GetTicks()) / 1000.0, 1000.0));
        if (textured_room) {
            draw_cameras(0);
            draw_decals(r, s, room, proj, 0, cams);
        } else {
            draw_geometry();
        }
        // the warps bend the level behind them, under the creatures
        draw_warp_tears(r, s, room, alpha, cam, proj, seconds);
        // the ground that isn't tiles, and the plants on it
        draw_flat([&] {
            build_sand(r, s, room, cam);
            build_plants(r, s, room, seconds);
        });

        // the sprite layers: rot tubes, back spears, creatures (midground), items, gate doors
        draw_flat([&] { build_rot(r, s, room, alpha, false); });
        draw_flat([&] { build_items(r, s, room, alpha, true); });
        r.batch.clear();
        bool const sprites = r.slugcat_texture != 0;
        if (sprites)
            build_slugcats(r, s, room, alpha);
        else
            build_slugcat_circles(r, s, room, alpha);
        size_t const bodies_end = r.batch.size();
        build_centipedes(r, s, room, alpha);
        build_items(r, s, room, alpha, false);
        build_gate_doors(r, s, room);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
        glUseProgram(r.program);
        glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
        if (sprites) {
            glBindTexture(GL_TEXTURE_2D, r.slugcat_texture);
            glUniform1i(r.u_textured, 1);
        }
        upload_and_draw(r, 0, bodies_end);
        glUniform1i(r.u_textured, 0);
        upload_and_draw(r, bodies_end, r.batch.size() - bodies_end);

        if (textured_room) {
            draw_cameras(1);
            draw_decals(r, s, room, proj, 1, cams);
            if (r.show_geometry)
                draw_geometry();
        }
        // most of the rot sits over the level's foreground (vanilla's Water container)
        draw_flat([&] { build_rot(r, s, room, alpha, true); });

        // mud over whatever sank into it
        draw_flat([&] { build_mud(r, s, room, seconds); });
        // drops, sparks, dust and bubbles, then shortcuts over the level, then
        // the water over everything below its surface
        draw_flat([&] {
            build_particles(r, s, room, alpha);
            build_shortcut_glow(r, s, room, alpha);
        });
        draw_water(r, s, room, alpha, cam, proj, seconds);

        draw_flat([&] { build_ripple_rings(r, s, room); });
        draw_warp_screen(r, s, room, alpha, cam, proj, seconds);

        if (r.show_hud)
            draw_meters(r, s);
        if (r.show_hud && r.font_texture != 0 && !hud.empty())
            draw_hud(r, hud);
    }

    void draw_hud(state& r, std::string const& hud) {
        constexpr float cw = 9.f;
        constexpr float ch = 16.f;
        constexpr float margin = 6.f;
        // panel sized to the longest line
        size_t lines = 1, longest = 0, cur = 0;
        for (char const c : hud) {
            if (c == '\n') {
                ++lines;
                cur = 0;
            } else {
                longest = std::max(longest, ++cur);
            }
        }
        r.batch.clear();
        painter p{r.batch};
        p.rect(0.f, 0.f, float(longest) * cw + margin * 2.f, float(lines) * ch + margin * 2.f, rgba(0, 0, 0, 160));
        size_t const untextured = r.batch.size();
        float const fu = cw / float(r.font_width);
        float const fv = ch / float(r.font_height);
        float x = margin, y = margin;
        for (char const c : hud) {
            if (c == '\n') {
                x = margin;
                y += ch;
                continue;
            }
            if (c > ' ' && c <= '~') {
                uint32_t const g = uint32_t(c - '!');
                float const u0 = float(g % 32) * fu;
                float const v0 = float(g / 32) * fv;
                p.glyph(x, y, cw, ch, u0, v0, u0 + fu, v0 + fv, rgba(230, 230, 230));
            }
            x += cw;
        }
        // screen space, y down
        glm::mat4 const proj = glm::ortho(0.f, float(r.width), float(r.height), 0.f);
        glUseProgram(r.program);
        glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
        glUniform1i(r.u_textured, 0);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(untextured));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, r.font_texture);
        glUniform1i(r.u_textured, 1);
        glDrawArrays(GL_TRIANGLES, GLint(untextured), GLsizei(r.batch.size() - untextured));
    }

    void present(state& r) {
        SDL_GL_SwapWindow(r.window);
    }

    void set_title(state& r, std::string const& title) {
        SDL_SetWindowTitle(r.window, title.c_str());
    }

    bool save_screenshot(state& r, char const* path) {
        int const w = r.width;
        int const h = r.height;
        std::vector<uint8_t> px(size_t(w) * size_t(h) * 3);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, w, h, GL_BGR, GL_UNSIGNED_BYTE, px.data());
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr)
            return false;
        // bottom up rows, which is what glReadPixels returns
        uint32_t const row = uint32_t(w) * 3u;
        uint32_t const pad = (4u - row % 4u) % 4u;
        uint32_t const data_size = (row + pad) * uint32_t(h);
        uint8_t hdr[54] = {'B', 'M'};
        auto const put32 = [&](int off, uint32_t v) {
            for (int i = 0; i < 4; ++i)
                hdr[off + i] = uint8_t(v >> (8 * i));
        };
        put32(2, 54 + data_size);
        put32(10, 54);
        put32(14, 40);
        put32(18, uint32_t(w));
        put32(22, uint32_t(h));
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, data_size);
        std::fwrite(hdr, 1, sizeof(hdr), f);
        uint8_t const zeros[3] = {0, 0, 0};
        for (int y = 0; y < h; ++y) {
            std::fwrite(px.data() + size_t(y) * row, 1, row, f);
            std::fwrite(zeros, 1, pad, f);
        }
        return std::fclose(f) == 0;
    }
}
