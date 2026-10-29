#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "unity_assets.hpp"

namespace unity {
    namespace {
        struct cursor {
            uint8_t const* p;
            size_t n;
            size_t i = 0;
            bool ok = true;
            template<typename T>
            T get() {
                T v{};
                if (i + sizeof(T) > n) {
                    ok = false;
                    return v;
                }
                std::memcpy(&v, p + i, sizeof(T));
                i += sizeof(T);
                return v;
            }
            void align() { i = (i + 3) & ~size_t(3); }
            void skip(size_t k) {
                if (i + k > n)
                    ok = false;
                i += k;
            }
            std::string str() {
                uint32_t const len = get<uint32_t>();
                if (!ok || i + len > n) {
                    ok = false;
                    return {};
                }
                std::string s(reinterpret_cast<char const*>(p + i), len);
                i += len;
                align();
                return s;
            }
        };

        bool read_file(std::string const& path, std::vector<uint8_t>& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            in.seekg(0, std::ios::end);
            auto const size = in.tellg();
            in.seekg(0);
            out.resize(size_t(size));
            in.read(reinterpret_cast<char*>(out.data()), size);
            return bool(in);
        }

        bool read_slice(std::string const& path, uint64_t offset, uint32_t size, std::vector<uint8_t>& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            in.seekg(std::streamoff(offset));
            out.resize(size);
            in.read(reinterpret_cast<char*>(out.data()), size);
            return bool(in);
        }

        // RGB565 to RGB8
        void rgb565(uint16_t c, uint8_t* o) {
            o[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
            o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
            o[2] = uint8_t((c & 31) * 255 / 31);
        }

        // a BC1 colour block into a 4x4 RGBA tile (alpha left alone unless `bc1_alpha`)
        void color_block(uint8_t const* b, uint8_t* tile, bool bc1_alpha) {
            uint16_t const c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
            uint8_t pal[4][4];
            rgb565(c0, pal[0]);
            rgb565(c1, pal[1]);
            pal[0][3] = pal[1][3] = 255;
            if (c0 > c1 || !bc1_alpha) {
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
                    pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
                }
                pal[2][3] = pal[3][3] = 255;
            } else {
                for (int k = 0; k < 3; ++k) {
                    pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
                    pal[3][k] = 0;
                }
                pal[2][3] = 255;
                pal[3][3] = 0;
            }
            uint32_t const bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
            for (int t = 0; t < 16; ++t) {
                uint8_t const* c = pal[(bits >> (2 * t)) & 3];
                tile[t * 4 + 0] = c[0];
                tile[t * 4 + 1] = c[1];
                tile[t * 4 + 2] = c[2];
                if (bc1_alpha)
                    tile[t * 4 + 3] = c[3];
            }
        }

        // a BC3 (DXT5) alpha block
        void alpha_block(uint8_t const* b, uint8_t* tile) {
            uint8_t a[8];
            a[0] = b[0];
            a[1] = b[1];
            if (a[0] > a[1]) {
                for (int k = 1; k < 7; ++k)
                    a[k + 1] = uint8_t(((7 - k) * a[0] + k * a[1]) / 7);
            } else {
                for (int k = 1; k < 5; ++k)
                    a[k + 1] = uint8_t(((5 - k) * a[0] + k * a[1]) / 5);
                a[6] = 0;
                a[7] = 255;
            }
            uint64_t bits = 0;
            for (int k = 0; k < 6; ++k)
                bits |= uint64_t(b[2 + k]) << (8 * k);
            for (int t = 0; t < 16; ++t)
                tile[t * 4 + 3] = a[(bits >> (3 * t)) & 7];
        }

        // TextureFormat to RGBA8, bottom row first like Unity stores it
        bool decode(int32_t format, int32_t w, int32_t h, std::vector<uint8_t> const& src, std::vector<uint8_t>& out) {
            out.assign(size_t(w) * size_t(h) * 4, 255);
            size_t const n = size_t(w) * size_t(h);
            switch (format) {
            case 1: // Alpha8
                if (src.size() < n)
                    return false;
                for (size_t i = 0; i < n; ++i)
                    out[i * 4 + 3] = src[i];
                return true;
            case 3: // RGB24
                if (src.size() < n * 3)
                    return false;
                for (size_t i = 0; i < n; ++i)
                    std::memcpy(&out[i * 4], &src[i * 3], 3);
                return true;
            case 4: // RGBA32
                if (src.size() < n * 4)
                    return false;
                std::memcpy(out.data(), src.data(), n * 4);
                return true;
            case 5: // ARGB32
                if (src.size() < n * 4)
                    return false;
                for (size_t i = 0; i < n; ++i) {
                    out[i * 4 + 0] = src[i * 4 + 1];
                    out[i * 4 + 1] = src[i * 4 + 2];
                    out[i * 4 + 2] = src[i * 4 + 3];
                    out[i * 4 + 3] = src[i * 4 + 0];
                }
                return true;
            case 10: // DXT1
            case 12: { // DXT5
                bool const dxt5 = format == 12;
                size_t const block = dxt5 ? 16 : 8;
                int32_t const bw = (w + 3) / 4, bh = (h + 3) / 4;
                if (src.size() < size_t(bw) * size_t(bh) * block)
                    return false;
                uint8_t tile[64];
                for (int32_t by = 0; by < bh; ++by)
                    for (int32_t bx = 0; bx < bw; ++bx) {
                        uint8_t const* b = &src[(size_t(by) * size_t(bw) + size_t(bx)) * block];
                        std::memset(tile, 255, sizeof(tile));
                        if (dxt5) {
                            alpha_block(b, tile);
                            color_block(b + 8, tile, false);
                        } else {
                            color_block(b, tile, true);
                        }
                        for (int ty = 0; ty < 4; ++ty)
                            for (int tx = 0; tx < 4; ++tx) {
                                int32_t const x = bx * 4 + tx, y = by * 4 + ty;
                                if (x < w && y < h)
                                    std::memcpy(&out[(size_t(y) * size_t(w) + size_t(x)) * 4], &tile[(ty * 4 + tx) * 4], 4);
                            }
                    }
                return true;
            }
            default:
                return false;
            }
        }
    }

    bool asset_library::open(std::string const& folder) {
        dir = folder;
        objects.clear();
        if (!read_file(dir + "/resources.assets", data) || data.size() < 64)
            return false;
        // SerializedFile header (big endian), version 22: 64 bit sizes
        auto const be32 = [&](size_t at) { return (uint32_t(data[at]) << 24) | (uint32_t(data[at + 1]) << 16) | (uint32_t(data[at + 2]) << 8) | uint32_t(data[at + 3]); };
        uint32_t const version = be32(8);
        if (version < 22)
            return false;
        uint64_t data_offset = 0;
        for (int k = 0; k < 8; ++k)
            data_offset = (data_offset << 8) | data[32 + size_t(k)];
        cursor c{data.data(), data.size(), 48};
        while (c.i < c.n && data[c.i] != 0)
            ++c.i;
        ++c.i;               // engine version
        c.get<int32_t>();    // platform
        bool const type_tree = c.get<uint8_t>() != 0;
        if (type_tree)
            return false;
        int32_t const ntypes = c.get<int32_t>();
        std::vector<int32_t> classes;
        for (int32_t k = 0; k < ntypes && c.ok; ++k) {
            int32_t const cls = c.get<int32_t>();
            c.skip(3);
            if (cls == 114)
                c.skip(16);
            c.skip(16);
            classes.push_back(cls);
        }
        int32_t const nobjects = c.get<int32_t>();
        for (int32_t k = 0; k < nobjects && c.ok; ++k) {
            c.align();
            c.get<int64_t>();
            uint64_t const start = c.get<uint64_t>();
            uint32_t const size = c.get<uint32_t>();
            int32_t const type = c.get<int32_t>();
            if (type >= 0 && size_t(type) < classes.size() && data_offset + start + size <= data.size())
                objects.push_back({classes[size_t(type)], data_offset + start, size});
        }
        return c.ok && !objects.empty();
    }

    bool asset_library::text(std::string_view name, std::string& out) const {
        for (auto const& o : objects) {
            if (o.cls != 49) // TextAsset
                continue;
            cursor c{data.data() + o.offset, o.size};
            if (c.str() != name)
                continue;
            out = c.str();
            return c.ok;
        }
        return false;
    }

    bool asset_library::texture(std::string_view name, image& out) const {
        for (auto const& o : objects) {
            if (o.cls != 28) // Texture2D
                continue;
            cursor c{data.data() + o.offset, o.size};
            if (c.str() != name)
                continue;
            // Texture2D, 2020.3
            c.get<int32_t>();  // forced fallback format
            c.skip(2);         // downscale fallback, alpha optional
            c.align();
            int32_t const w = c.get<int32_t>();
            int32_t const h = c.get<int32_t>();
            c.get<int32_t>();  // complete image size
            c.get<int32_t>();  // mips stripped
            int32_t const format = c.get<int32_t>();
            c.get<int32_t>();  // mip count
            c.skip(4);         // readable, preprocessed, ignore limit, streaming mips
            c.align();
            c.get<int32_t>();  // streaming priority
            c.get<int32_t>();  // image count
            c.get<int32_t>();  // dimension
            c.skip(24);        // GLTextureSettings
            c.skip(8);         // lightmap format, colour space
            c.skip(c.get<uint32_t>()); // platform blob
            c.align();
            uint32_t const inline_size = c.get<uint32_t>();
            std::vector<uint8_t> pixels;
            if (inline_size > 0) {
                if (c.i + inline_size > c.n)
                    return false;
                pixels.assign(c.p + c.i, c.p + c.i + inline_size);
                c.skip(inline_size);
                c.align();
            } else {
                c.align();
                uint64_t const offset = c.get<uint64_t>();
                uint32_t const size = c.get<uint32_t>();
                std::string const path = c.str();
                if (!c.ok || path.empty() || !read_slice(dir + "/" + path, offset, size, pixels))
                    return false;
            }
            std::vector<uint8_t> rgba;
            if (!c.ok || !decode(format, w, h, pixels, rgba))
                return false;
            // Unity keeps the bottom row first
            out.name = std::string{name};
            out.width = w;
            out.height = h;
            out.rgba.resize(rgba.size());
            size_t const row = size_t(w) * 4;
            for (int32_t y = 0; y < h; ++y)
                std::memcpy(&out.rgba[size_t(y) * row], &rgba[size_t(h - 1 - y) * row], row);
            return true;
        }
        return false;
    }

    std::string find_asset_dir(std::vector<std::string> const& roots, std::string const& explicit_dir) {
        namespace fs = std::filesystem;
        auto const has = [](fs::path const& p) {
            std::error_code ec;
            return fs::exists(p / "resources.assets", ec);
        };
        if (!explicit_dir.empty())
            return explicit_dir;
        if (char const* env = std::getenv("RW_UNITY"); env != nullptr && *env)
            return env;
        // RainWorld_Data/StreamingAssets: the Unity data is one folder up
        for (auto const& r : roots) {
            fs::path const parent = fs::path(r).parent_path();
            if (has(parent))
                return parent.string();
        }
        if (has("unity-assets/staging"))
            return "unity-assets/staging";
        return {};
    }
}
