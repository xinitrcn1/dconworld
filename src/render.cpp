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
#include "gates.hpp"
#include "items.hpp"
#include "physics.hpp"
#include "player_graphics.hpp"
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
                if (world.physical_object_get_room_from_object_location(o) != room.id || world.player_get_in_shortcut(pid))
                    return;
                cmds.clear();
                player_graphics::build(s, pid, alpha, cmds);
                for (auto const& c : cmds) {
                    if (c.is_triangle) {
                        uint32_t const col = to_rgba(c.color);
                        for (auto const& v : c.tri)
                            r.batch.push_back(vertex{v.x, v.y, col, white_uv.x, white_uv.y});
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
                    uint32_t const col = to_rgba(c.spr.color);
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
                if (world.physical_object_get_room_from_object_location(o) != room.id || world.player_get_in_shortcut(pid))
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
        void build_items(state& r, sys::game_state const& s, world::room_view const& room, float alpha) {
            auto const& world = s.world;
            painter p{r.batch};
            world.for_each_item([&](dcon::item_id it) {
                auto const o = items::body(s, it);
                if (world.physical_object_get_room_from_object_location(o) != room.id)
                    return;
                auto const g = items::grabber(s, it);
                if (g && world.player_get_in_shortcut(g))
                    return;
                auto const c = items::chunk(s, it);
                vec2 const pos = rw::lerp(world.body_chunk_get_last_pos(c), world.body_chunk_get_pos(c), alpha);
                if (world.item_get_type(it) == world::item_type::spear) {
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

        if (textured_room)
            draw_cameras(0);
        else
            draw_geometry();

        // the sprite layers: creatures (midground), items, gate doors
        r.batch.clear();
        bool const sprites = r.slugcat_texture != 0;
        if (sprites)
            build_slugcats(r, s, room, alpha);
        else
            build_slugcat_circles(r, s, room, alpha);
        size_t const bodies_end = r.batch.size();
        build_items(r, s, room, alpha);
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
            if (r.show_geometry)
                draw_geometry();
        }

        // shortcuts are drawn over the level, then water over everything below the surface
        draw_flat([&] {
            build_shortcut_glow(r, s, room, alpha);
            if (room.water) {
                painter p{r.batch};
                p.rect(-200.f, -200.f, room.pixel_width() + 400.f, room.float_water_level + 200.f, col_water);
            }
        });

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
