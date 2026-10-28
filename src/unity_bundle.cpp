#include <algorithm>
#include <cstring>
#include <utility>

#include "unity_bundle.hpp"

namespace unity {
    namespace {
        // LZ4 block format (no frame), bounds checked
        bool lz4_decompress(uint8_t const* src, size_t n, uint8_t* dst, size_t usize) {
            size_t i = 0, o = 0;
            while (i < n) {
                uint8_t const tok = src[i++];
                size_t lit = tok >> 4;
                if (lit == 15) {
                    uint8_t b;
                    do {
                        if (i >= n)
                            return false;
                        b = src[i++];
                        lit += b;
                    } while (b == 255);
                }
                if (i + lit > n || o + lit > usize)
                    return false;
                std::memcpy(dst + o, src + i, lit);
                i += lit;
                o += lit;
                if (i >= n)
                    break;
                if (i + 2 > n)
                    return false;
                size_t const off = size_t(src[i]) | (size_t(src[i + 1]) << 8);
                i += 2;
                size_t ml = tok & 15;
                if (ml == 15) {
                    uint8_t b;
                    do {
                        if (i >= n)
                            return false;
                        b = src[i++];
                        ml += b;
                    } while (b == 255);
                }
                ml += 4;
                if (off == 0 || off > o || o + ml > usize)
                    return false;
                // overlapping copies repeat the pattern: byte by byte
                for (size_t k = 0; k < ml; ++k, ++o)
                    dst[o] = dst[o - off];
            }
            return o == usize;
        }

        struct reader {
            uint8_t const* p;
            size_t n;
            size_t i = 0;
            bool big = true;
            bool ok = true;

            bool need(size_t k) {
                if (i + k > n)
                    ok = false;
                return ok;
            }
            template<typename T>
            T get() {
                T v{};
                if (!need(sizeof(T)))
                    return v;
                uint8_t b[sizeof(T)];
                std::memcpy(b, p + i, sizeof(T));
                i += sizeof(T);
                if (big)
                    for (size_t k = 0; k < sizeof(T) / 2; ++k)
                        std::swap(b[k], b[sizeof(T) - 1 - k]);
                std::memcpy(&v, b, sizeof(T));
                return v;
            }
            std::string cstr() {
                size_t j = i;
                while (j < n && p[j] != 0)
                    ++j;
                if (j >= n) {
                    ok = false;
                    return {};
                }
                std::string s{reinterpret_cast<char const*>(p + i), j - i};
                i = j + 1;
                return s;
            }
            // length prefixed, padded to 4 bytes
            std::string str() {
                uint32_t const len = get<uint32_t>();
                if (!need(len))
                    return {};
                std::string s{reinterpret_cast<char const*>(p + i), len};
                i += len;
                align();
                return s;
            }
            void align() { i = (i + 3) & ~size_t(3); }
            void skip(size_t k) {
                if (need(k))
                    i += k;
            }
        };
    }

    bool bundle::open(layered_fs::mapped_file&& f) {
        file = std::move(f);
        reader r{static_cast<uint8_t const*>(file.base), file.size};
        if (r.cstr() != "UnityFS")
            return false;
        uint32_t const version = r.get<uint32_t>();
        r.cstr(); // "5.x.x"
        r.cstr(); // engine version
        int64_t const total = r.get<int64_t>();
        uint32_t const csize = r.get<uint32_t>();
        uint32_t const usize = r.get<uint32_t>();
        uint32_t const flags = r.get<uint32_t>();
        if (!r.ok || uint64_t(total) > file.size)
            return false;
        if (version >= 7)
            r.i = (r.i + 15) & ~size_t(15);
        size_t info_at = r.i;
        size_t data_at = r.i + csize;
        if (flags & 0x80) { // block info at the end
            info_at = size_t(total) - csize;
            data_at = r.i;
        }
        if (flags & 0x200)
            data_at = (data_at + 15) & ~size_t(15);
        if (info_at + csize > file.size)
            return false;
        std::vector<uint8_t> info(usize);
        auto const* src = static_cast<uint8_t const*>(file.base) + info_at;
        uint32_t const comp = flags & 0x3f;
        if (comp == 0) {
            if (csize != usize)
                return false;
            std::memcpy(info.data(), src, usize);
        } else if (comp == 2 || comp == 3) {
            if (!lz4_decompress(src, csize, info.data(), usize))
                return false;
        } else {
            return false; // LZMA bundles aren't used by the game
        }
        reader b{info.data(), info.size()};
        b.skip(16); // uncompressed data hash
        uint32_t const nblocks = b.get<uint32_t>();
        uint64_t uoff = 0, coff = data_at;
        for (uint32_t k = 0; k < nblocks && b.ok; ++k) {
            block bl;
            bl.usize = b.get<uint32_t>();
            bl.csize = b.get<uint32_t>();
            bl.flags = b.get<uint16_t>();
            bl.uoff = uoff;
            bl.coff = coff;
            uoff += bl.usize;
            coff += bl.csize;
            blocks.push_back(bl);
        }
        uint32_t const nnodes = b.get<uint32_t>();
        for (uint32_t k = 0; k < nnodes && b.ok; ++k) {
            node nd;
            nd.offset = b.get<uint64_t>();
            nd.size = b.get<uint64_t>();
            nd.flags = b.get<uint32_t>();
            nd.name = b.cstr();
            nodes.push_back(std::move(nd));
        }
        return b.ok && coff <= file.size;
    }

    bool bundle::read(uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const {
        out.resize(size_t(size));
        uint64_t done = 0;
        // blocks are in order: binary search the first one
        size_t lo = 0, hi = blocks.size();
        while (lo < hi) {
            size_t const mid = (lo + hi) / 2;
            if (blocks[mid].uoff + blocks[mid].usize <= offset)
                lo = mid + 1;
            else
                hi = mid;
        }
        for (size_t k = lo; k < blocks.size() && done < size; ++k) {
            auto const& bl = blocks[k];
            if (int64_t(k) != cached) {
                cache.resize(bl.usize);
                auto const* src = static_cast<uint8_t const*>(file.base) + bl.coff;
                uint32_t const comp = bl.flags & 0x3f;
                if (comp == 0) {
                    if (bl.csize != bl.usize)
                        return false;
                    std::memcpy(cache.data(), src, bl.usize);
                } else if (comp == 2 || comp == 3) {
                    if (!lz4_decompress(src, bl.csize, cache.data(), bl.usize))
                        return false;
                } else {
                    return false;
                }
                cached = int64_t(k);
            }
            uint64_t const at = offset + done;
            uint64_t const from = at - bl.uoff;
            uint64_t const take = std::min<uint64_t>(bl.usize - from, size - done);
            std::memcpy(out.data() + done, cache.data() + from, size_t(take));
            done += take;
        }
        return done == size;
    }

    bool audio_clips(bundle const& b, std::vector<audio_clip>& out, uint64_t& resource_base) {
        if (b.nodes.size() < 2)
            return false;
        // node 0: the serialized file, node 1: "<cab>.resource"
        std::vector<uint8_t> sf;
        if (!b.read(b.nodes[0].offset, b.nodes[0].size, sf))
            return false;
        resource_base = b.nodes[1].offset;
        reader r{sf.data(), sf.size()};
        r.get<uint32_t>(); // metadata size
        r.get<uint32_t>(); // file size
        uint32_t const version = r.get<uint32_t>();
        uint64_t data_offset = r.get<uint32_t>();
        uint8_t const endian = r.get<uint8_t>();
        r.skip(3);
        if (version >= 22) {
            r.get<uint32_t>();
            r.get<uint64_t>();
            data_offset = r.get<uint64_t>();
            r.skip(8);
        }
        if (version < 19)
            return false; // older layouts aren't needed
        r.big = endian != 0;
        r.cstr(); // engine version
        r.get<int32_t>(); // platform
        bool const type_tree = r.get<uint8_t>() != 0;
        int32_t const ntypes = r.get<int32_t>();
        std::vector<int32_t> classes;
        for (int32_t k = 0; k < ntypes && r.ok; ++k) {
            int32_t const cls = r.get<int32_t>();
            r.skip(1); // stripped
            r.get<int16_t>(); // script type index
            if (cls == 114)
                r.skip(16); // script id
            r.skip(16); // type hash
            if (type_tree) {
                int32_t const nnodes = r.get<int32_t>();
                int32_t const strings = r.get<int32_t>();
                r.skip(size_t(nnodes) * 32 + size_t(strings));
                if (version >= 21) {
                    int32_t const deps = r.get<int32_t>();
                    r.skip(size_t(deps) * 4);
                }
            }
            classes.push_back(cls);
        }
        int32_t const nobjects = r.get<int32_t>();
        for (int32_t k = 0; k < nobjects && r.ok; ++k) {
            r.align();
            r.get<int64_t>(); // path id
            uint64_t const start = version >= 22 ? uint64_t(r.get<int64_t>()) : r.get<uint32_t>();
            uint32_t const size = r.get<uint32_t>();
            int32_t const type = r.get<int32_t>();
            if (!r.ok || type < 0 || size_t(type) >= classes.size() || classes[size_t(type)] != 83)
                continue;
            uint64_t const at = data_offset + start;
            if (at + size > sf.size())
                continue;
            // AudioClip (2019.3+ layout)
            reader o{sf.data() + at, size, 0, r.big};
            audio_clip c;
            c.name = o.str();
            o.get<int32_t>(); // load type
            c.channels = o.get<int32_t>();
            c.frequency = o.get<int32_t>();
            o.get<int32_t>(); // bits per sample
            o.get<float>();   // length
            o.skip(2);        // tracker format, ambisonic
            o.align();
            o.get<int32_t>(); // subsound index
            o.skip(3);        // preload, load in background, legacy 3d
            o.align();
            o.str(); // resource source name
            c.offset = o.get<uint64_t>();
            c.size = o.get<uint64_t>();
            c.compression = o.get<int32_t>();
            if (o.ok)
                out.push_back(std::move(c));
        }
        return r.ok;
    }
}
