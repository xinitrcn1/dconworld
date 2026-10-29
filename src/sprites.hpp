#pragma once

// What the ported DrawSprites functions hand the renderer: Futile sprites
// (by element name, looked up in an atlas) and TriangleMesh triangles, in
// room space and in vanilla sprite order.

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
}
