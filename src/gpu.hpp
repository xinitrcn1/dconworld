#pragma once

// A small immediate mode layer over Vulkan 1.3 for render.cpp: textures by
// id, one vertex format, a fixed set of pipelines (src/shaders, compiled to
// SPIR-V at build time) and a frame drawn into an offscreen image, which
// is what the frame-so-far copies (grab_scene), read backs and screenshots
// read, and which present() blits to the window's swapchain.
//
// Coordinates follow OpenGL: the viewport is flipped so that the shaders'
// clip space has y up, and grab_scene's copy is bottom-up like
// glCopyTexSubImage2D's was.

#include <cstddef>
#include <cstdint>
#include <initializer_list>

struct SDL_Window;

namespace gpu {
    struct vertex {
        float x, y;
        uint32_t rgba; // little endian 0xAABBGGRR
        float u, v;
    };

    // What the vertex pulling pipelines (rope, circle) read out of the
    // storage buffer (binding 4, std430), see src/shaders/rope.vert and
    // circle.vert
    struct rope_point {
        float x, y;
        float rad;     // half width (< 0: padding, the point before a rope's first and after its last)
        uint32_t rgba; // little endian 0xAABBGGRR
    };
    struct circle {
        float x, y;
        float rad;
        uint32_t rgba;
        float rotation; // degrees, clockwise from up (Custom.DegToVec)
        float jag;      // 0: round, else a rim of 18 corners at rad * (1 - jag + jag * roll)
        uint32_t seed;  // the corners' rolls (xorshift32)
        float pad;
    };
    static_assert(sizeof(rope_point) == 16 && sizeof(circle) == 32);

    struct context; // opaque

    // 0 is "no texture"
    using texture = uint32_t;

    enum class sampler : uint8_t {
        nearest_clamp,
        nearest_repeat,
        linear_clamp,
    };

    // one per vertex/fragment shader pair; the uniform block (set 0,
    // binding 0) is each shader's own, textures are bindings 1..3, the
    // storage buffer (upload_storage) binding 4
    enum class pipeline : uint8_t {
        flat,        // flat.vert/frag: coloured, optionally textured (1: the texture)
        room,        // room.vert/frag: the level shader (1: cameras, 2: palettes, 3: effect colours)
        decal,       // decal.vert/frag (1: cameras, 2: palettes, 3: the decal)
        warp_tear,   // warp.vert + warp_tear.frag (1: the scene)
        warp_screen, // warp.vert + warp_screen.frag (1: the scene)
        water,       // warp.vert + water.frag (1: the scene)
        rope,        // rope.vert/frag: ribbons along rope_points (no vertex input: draw_pulled)
        circle,      // circle.vert/frag: discs, a quad each (no vertex input: draw_pulled)
        count,
    };

    // `present`: a window to show frames in (else frames are only read back)
    context* create(SDL_Window* window, bool present);
    void destroy(context* c);

    // RGBA8 texture, `layers` 0 for a plain 2D one (sampler2D) or a layer
    // count for an array (sampler2DArray); its texels start black. 0 on failure
    texture create_texture(context& c, int width, int height, int layers, sampler s);
    // `channels` 3 (RGB, alpha 1 like GL_RGB8) or 4, rows top to bottom
    void upload(context& c, texture t, int layer, uint8_t const* pixels, int channels);
    void destroy_texture(context& c, texture t);
    uint32_t max_texture_size(context const& c);

    // starts a frame of `width` x `height` pixels cleared to the colour
    void begin_frame(context& c, int width, int height, float r, float g, float b);
    // the vertices the following draws read (until the next upload)
    void upload_vertices(context& c, vertex const* v, size_t count);
    // `uniforms`: the pipeline's uniform block, std140 (at most 1 KiB)
    void draw(context& c, pipeline p, void const* uniforms, size_t size, std::initializer_list<texture> textures, size_t first, size_t count);
    // data for the following draws' storage buffer (binding 4: the frame's,
    // what the vertex pulling pipelines read); the byte offset it went to,
    // a multiple of 32 (a whole number of rope_points or circles), which the
    // draw's uniforms then pass on. Upload all of a pass's before its draws:
    // a later upload may move the buffer (the earlier draws keep reading the old)
    size_t upload_storage(context& c, void const* data, size_t bytes);
    // `vertices` vertices without vertex input (the shader pulls its data
    // out of the storage buffer by gl_VertexIndex), triangle list
    void draw_pulled(context& c, pipeline p, void const* uniforms, size_t size, std::initializer_list<texture> textures, size_t vertices);
    // a copy of the frame drawn so far (Unity's GrabPass), bottom row first
    // like glCopyTexSubImage2D's, linearly filtered, alpha 1
    texture grab_scene(context& c);
    // the frame drawn so far, RGBA, width * height texels, rows top to bottom
    // (unlike glReadPixels); waits for the GPU. Stays valid until the next draw
    uint8_t const* read_back(context& c);
    // ends the frame and shows it (if presenting)
    void present(context& c);
}
