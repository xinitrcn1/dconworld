#pragma once

// What the ported DrawSprites functions hand the renderer: Futile sprites
// (by element name, looked up in an atlas) and TriangleMesh triangles, in
// room space and in vanilla sprite order.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "math.hpp"

namespace sprites {
    // One FSprite: rotation is Futile's (degrees, clockwise), anchor is
    // relative to the element size like FSprite.anchorX/anchorY
    struct sprite {
        std::string_view element;
        rw::vec2 pos;
        float rotation = 0.f;
        rw::vec2 scale{1.f, 1.f};
        rw::vec2 anchor{0.5f, 0.5f};
        rw::vec3 color{1.f, 1.f, 1.f};
    };
    // A draw command: either a sprite or an untextured triangle (a
    // TriangleMesh piece)
    struct draw_cmd {
        bool is_triangle = false;
        sprite spr;
        rw::vec2 tri[3];
        rw::vec3 color{1.f, 1.f, 1.f};
        // per vertex colours with alpha: the triangle's corners, or a
        // VertexColorSprite's top left, top right, bottom right, bottom left
        bool vertex_colors = false;
        rw::vec4 vcol[4];
        // a CustomFSprite: the sprite's element stretched over four free
        // corners in the same order (spr's position, rotation and scale unused)
        bool is_quad = false;
        rw::vec2 quad[4];
        // a whole TriangleMesh.MakeLongMesh: triangles (k, k + 1, k + 2) over
        // the strip vertices [strip_first, strip_first + strip_count) kept
        // beside the commands (one command instead of one per triangle)
        bool is_strip = false;
        uint32_t strip_first = 0;
        uint32_t strip_count = 0;
    };
    // a strip's vertex: its position and colour with alpha
    struct strip_vertex {
        rw::vec2 pos;
        rw::vec4 col;
    };

    inline void add(std::vector<draw_cmd>& out, sprite const& s) {
        draw_cmd c;
        c.spr = s;
        out.push_back(c);
    }
    // VertexColorSprite
    inline void add(std::vector<draw_cmd>& out, sprite const& s, rw::vec4 const (&corners)[4]) {
        draw_cmd c;
        c.spr = s;
        c.vertex_colors = true;
        for (int i = 0; i < 4; ++i)
            c.vcol[i] = corners[i];
        out.push_back(c);
    }
    // CustomFSprite (MoveVertice 0..3, verticeColors)
    inline void add_quad(std::vector<draw_cmd>& out, std::string_view element, rw::vec2 const (&corners)[4], rw::vec4 const (&colors)[4]) {
        draw_cmd c;
        c.spr.element = element;
        c.is_quad = true;
        c.vertex_colors = true;
        for (int i = 0; i < 4; ++i) {
            c.quad[i] = corners[i];
            c.vcol[i] = colors[i];
        }
        out.push_back(c);
    }
    // TriangleMesh.MakeLongMesh: triangles (k, k + 1, k + 2) over the vertices
    inline void long_mesh(std::vector<draw_cmd>& out, rw::vec2 const* v, size_t n, rw::vec3 color) {
        for (size_t k = 0; k + 2 < n; ++k) {
            draw_cmd c;
            c.is_triangle = true;
            c.tri[0] = v[k];
            c.tri[1] = v[k + 1];
            c.tri[2] = v[k + 2];
            c.color = color;
            out.push_back(c);
        }
    }
    // a long mesh as one strip command, its vertices appended to `strip`
    // (the renderer then needs `strip` with the commands)
    inline void long_mesh(std::vector<draw_cmd>& out, std::vector<strip_vertex>& strip, rw::vec2 const* v, rw::vec4 const* colors, size_t n) {
        if (n < 3)
            return;
        draw_cmd c;
        c.is_strip = true;
        c.strip_first = uint32_t(strip.size());
        c.strip_count = uint32_t(n);
        for (size_t i = 0; i < n; ++i)
            strip.push_back(strip_vertex{v[i], colors[i]});
        out.push_back(c);
    }
    inline void long_mesh(std::vector<draw_cmd>& out, std::vector<strip_vertex>& strip, rw::vec2 const* v, size_t n, rw::vec3 color) {
        if (n < 3)
            return;
        draw_cmd c;
        c.is_strip = true;
        c.strip_first = uint32_t(strip.size());
        c.strip_count = uint32_t(n);
        rw::vec4 const col{color.x, color.y, color.z, 1.f};
        for (size_t i = 0; i < n; ++i)
            strip.push_back(strip_vertex{v[i], col});
        out.push_back(c);
    }
    // the same with TriangleMesh.verticeColors
    inline void long_mesh(std::vector<draw_cmd>& out, rw::vec2 const* v, rw::vec4 const* colors, size_t n) {
        for (size_t k = 0; k + 2 < n; ++k) {
            draw_cmd c;
            c.is_triangle = true;
            c.vertex_colors = true;
            for (size_t i = 0; i < 3; ++i) {
                c.tri[i] = v[k + i];
                c.vcol[i] = colors[k + i];
            }
            out.push_back(c);
        }
    }
    // What an ApplyPalette reads of the room's palette (RoomPalette): the
    // palette image's texels (RoomPalette's texture, 32 x 16, rows counted
    // from the bottom like Texture2D.GetPixel), none without one
    struct room_palette {
        // the palette's RGB pixels, top row first (nullptr: no palette)
        uint8_t const* pixels = nullptr;
        // the room's effect colours (EffectColorA/B: the effect colour
        // palette's texels), white without them
        rw::vec3 effect_a{1.f, 1.f, 1.f};
        rw::vec3 effect_b{1.f, 1.f, 1.f};
        static constexpr int width = 32;
        static constexpr int height = 16;
        // texture.GetPixel(x, y)
        rw::vec3 texel(int x, int y) const noexcept {
            if (pixels == nullptr || x < 0 || y < 0 || x >= width || y >= height)
                return rw::vec3{0.f, 0.f, 0.f};
            uint8_t const* q = pixels + (size_t(height - 1 - y) * width + size_t(x)) * 3;
            return rw::vec3{float(q[0]) / 255.f, float(q[1]) / 255.f, float(q[2]) / 255.f};
        }
        // RoomPalette.blackColor, fogColor, skyColor, darkness
        rw::vec3 black() const noexcept { return texel(2, 7); }
        rw::vec3 fog() const noexcept { return texel(1, 7); }
        rw::vec3 sky() const noexcept { return texel(0, 7); }
        float darkness() const noexcept { return 1.f - texel(30, 7).x; }
    };
}
