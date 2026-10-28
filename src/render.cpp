#include <algorithm>
#include <cmath>
#include <cstdio>

#include <GL/glew.h>
#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

#include "render.hpp"
#include "physics.hpp"
#include "reports.hpp"

namespace render {
    using rw::vec2;
    using world::terrain;

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

    bool load_font(state& r, layered_fs::filesystem const& fs) {
        // anthiel's VGA font: 9x16 cells, 32 per row, starting at '!'
        auto const of = fs.get_root().get_single_file("vgafont.png").open_file(fs);
        if (!of) {
            reports::add_log("can't find vgafont.png, the HUD is disabled");
            return false;
        }
        auto const sv = of->to_string_view();
        int w = 0, h = 0, n = 0;
        stbi_uc* px = stbi_load_from_memory(reinterpret_cast<stbi_uc const*>(sv.data()), int(sv.size()), &w, &h, &n, 4);
        if (px == nullptr) {
            reports::add_log(std::string{"vgafont.png: "} + stbi_failure_reason());
            return false;
        }
        glGenTextures(1, &r.font_texture);
        glBindTexture(GL_TEXTURE_2D, r.font_texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(px);
        r.font_width = w;
        r.font_height = h;
        return true;
    }

    void shutdown(state& r) {
        if (r.context != nullptr) {
            glDeleteBuffers(1, &r.vbo);
            glDeleteVertexArrays(1, &r.vao);
            glDeleteProgram(r.program);
            glDeleteTextures(1, &r.font_texture);
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

    void draw_frame(state& r, sys::game_state const& s, world::room_view const& room, float alpha, camera const& cam, std::string const& hud) {
        auto const& world = s.world;
        SDL_GetWindowSizeInPixels(r.window, &r.width, &r.height);
        glViewport(0, 0, r.width, r.height);
        glClearColor(float(col_background & 0xFF) / 255.f, float((col_background >> 8) & 0xFF) / 255.f, float((col_background >> 16) & 0xFF) / 255.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);

        r.batch.clear();
        painter p{r.batch};
        float const view_w = float(r.width) / cam.zoom;
        float const view_h = float(r.height) / cam.zoom;

        // visible tile range
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
                            p.rect(fx, fy, 20.f, 20.f, col_wall_behind);
                    } else if (pass == 1) {
                        if (t.t == terrain::solid) {
                            p.rect(fx, fy, 20.f, 20.f, col_solid);
                        } else if (t.t == terrain::slope) {
                            // triangle filling the corner between the two solid neighbours
                            vec2 const bl{fx, fy}, br{fx + 20.f, fy}, tl{fx, fy + 20.f}, tr{fx + 20.f, fy + 20.f};
                            switch (room.identify_slope(x, y)) {
                            case world::slope_direction::up_right: p.tri(bl, br, tl, col_solid); break;   // solid left & below
                            case world::slope_direction::up_left: p.tri(bl, br, tr, col_solid); break;    // solid right & below
                            case world::slope_direction::down_right: p.tri(tl, tr, bl, col_solid); break; // solid left & above
                            case world::slope_direction::down_left: p.tri(tl, tr, br, col_solid); break;  // solid right & above
                            default: p.tri(bl, br, vec2{fx + 10.f, fy + 10.f}, col_solid); break;
                            }
                        }
                    } else {
                        if (t.t == terrain::floor)
                            p.rect(fx, fy + 15.f, 20.f, 5.f, col_floor);
                        if (t.vertical_beam())
                            p.rect(fx + 8.5f, fy, 3.f, 20.f, col_beam);
                        if (t.horizontal_beam())
                            p.rect(fx, fy + 8.5f, 20.f, 3.f, col_beam);
                        if (t.shortcut != 0 && t.t != terrain::shortcut_entrance)
                            p.rect(fx + 7.f, fy + 7.f, 6.f, 6.f, col_shortcut_path);
                        if (t.t == terrain::shortcut_entrance)
                            p.rect(fx + 2.f, fy + 2.f, 16.f, 16.f, col_shortcut_entrance);
                    }
                }
            }
        }

        // room outline and vanilla camera views
        float const line_w = 1.f / cam.zoom;
        p.outline(0.f, 0.f, room.pixel_width(), room.pixel_height(), line_w, col_bounds);
        if (r.show_cameras) {
            world.room_for_each_camera_location(room.id, [&](dcon::camera_location_id cl) {
                vec2 const cp = world.room_camera_get_position(world.camera_location_get_camera(cl));
                p.outline(cp.x, cp.y, 1366.f, 768.f, line_w, col_camera);
            });
        }

        // every player whose body is in this room
        world.for_each_player([&](dcon::player_id pid) {
            auto const o = world.player_get_body_from_player_body(pid);
            if (world.physical_object_get_room_from_object_location(o) != room.id)
                return;
            auto const c0 = physics::chunk(s, o, 0);
            auto const c1 = physics::chunk(s, o, 1);
            vec2 const b0 = rw::lerp(world.body_chunk_get_last_pos(c0), world.body_chunk_get_pos(c0), alpha);
            vec2 const b1 = rw::lerp(world.body_chunk_get_last_pos(c1), world.body_chunk_get_pos(c1), alpha);
            uint32_t const body = world.player_get_dead(pid) ? col_body_dead : col_body;
            p.line(b0, b1, 2.f, body);
            p.circle(b1, world.body_chunk_get_rad(c1), body);
            p.circle(b0, world.body_chunk_get_rad(c0), body);
            // eye, showing the facing direction
            p.circle(b0 + vec2{float(world.player_get_flip_direction(pid)) * 4.f, 3.f}, 2.f, col_eye);
        });

        // water drawn over everything below the surface
        if (room.water)
            p.rect(-200.f, -200.f, room.pixel_width() + 400.f, room.float_water_level + 200.f, col_water);

        glm::mat4 const proj = glm::ortho(cam.pos.x, cam.pos.x + view_w, cam.pos.y, cam.pos.y + view_h);
        glUseProgram(r.program);
        glUniformMatrix4fv(r.u_proj, 1, GL_FALSE, glm::value_ptr(proj));
        glUniform1i(r.u_textured, 0);
        glBindVertexArray(r.vao);
        glBindBuffer(GL_ARRAY_BUFFER, r.vbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(r.batch.size() * sizeof(vertex)), r.batch.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(r.batch.size()));

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
