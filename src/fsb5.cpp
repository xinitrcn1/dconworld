#include <cstring>
#include <span>

#include "fsb5.hpp"

#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

// vcb_list: FMOD's Vorbis setup headers keyed by CRC32 (vgmstream)
#include "vorbis_codebooks_fsb.h"

namespace fsb5 {
    namespace {
        uint32_t u32(uint8_t const* p) {
            return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        }
        uint64_t u64(uint8_t const* p) {
            return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32);
        }

        struct ogg_writer {
            std::vector<uint8_t>& out;
            uint32_t seq = 0;
            static constexpr uint32_t serial = 0x52574350; // "RWCP"

            static uint32_t const* crc_table() {
                static uint32_t table[256] = {};
                static bool init = false;
                if (!init) {
                    for (uint32_t i = 0; i < 256; ++i) {
                        uint32_t r = i << 24;
                        for (int k = 0; k < 8; ++k)
                            r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : r << 1;
                        table[i] = r;
                    }
                    init = true;
                }
                return table;
            }

            // one page holding whole packets (at most 255 lacing values)
            void page(std::span<std::span<uint8_t const> const> packets, uint8_t type, int64_t granule) {
                std::vector<uint8_t> lacing;
                for (auto const& pk : packets) {
                    size_t n = pk.size();
                    while (n >= 255) {
                        lacing.push_back(255);
                        n -= 255;
                    }
                    lacing.push_back(uint8_t(n));
                }
                size_t const start = out.size();
                uint8_t h[27] = {'O', 'g', 'g', 'S', 0, type};
                std::memcpy(h + 6, &granule, 8);
                std::memcpy(h + 14, &serial, 4);
                std::memcpy(h + 18, &seq, 4);
                h[26] = uint8_t(lacing.size());
                out.insert(out.end(), h, h + 27);
                out.insert(out.end(), lacing.begin(), lacing.end());
                for (auto const& pk : packets)
                    out.insert(out.end(), pk.begin(), pk.end());
                uint32_t crc = 0;
                auto const* t = crc_table();
                for (size_t i = start; i < out.size(); ++i)
                    crc = (crc << 8) ^ t[((crc >> 24) & 0xff) ^ out[i]];
                std::memcpy(out.data() + start + 22, &crc, 4);
                ++seq;
            }
        };

        uint32_t lacing_count(size_t n) { return uint32_t(n / 255 + 1); }
    }

    bool parse(uint8_t const* d, size_t size, info& out) {
        if (size < 0x3C || std::memcmp(d, "FSB5", 4) != 0)
            return false;
        uint32_t const version = u32(d + 4);
        uint32_t const sample_headers = u32(d + 12);
        uint32_t const names = u32(d + 16);
        uint32_t const data_size = u32(d + 20);
        out.mode = u32(d + 24);
        size_t const header = version == 0 ? 0x40 : 0x3C;
        if (header + 8 > size)
            return false;
        uint64_t const raw = u64(d + header);
        static constexpr int rates[] = {4000, 8000, 11000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 96000};
        uint32_t const rate_index = uint32_t(raw >> 1) & 0xF;
        out.rate = rate_index < 11 ? rates[rate_index] : 44100;
        out.channels = int((raw >> 5) & 1) + 1;
        size_t const offset = size_t((raw >> 6) & 0x0FFFFFFF) * 16;
        out.samples = uint32_t(raw >> 34) & 0x3FFFFFFF;
        bool more = raw & 1;
        size_t p = header + 8;
        while (more && p + 4 <= size) {
            uint32_t const c = u32(d + p);
            p += 4;
            more = c & 1;
            uint32_t const len = (c >> 1) & 0xFFFFFF;
            uint32_t const type = (c >> 25) & 0x7F;
            if (p + len > size)
                return false;
            if (type == 1 && len >= 1)
                out.channels = d[p];
            else if (type == 2 && len >= 4)
                out.rate = int(u32(d + p));
            else if (type == 11 && len >= 4)
                out.vorbis_crc = u32(d + p);
            p += len;
        }
        out.data_offset = header + sample_headers + names + offset;
        out.data_size = data_size - offset;
        if (out.data_offset > size)
            return false;
        out.data_size = std::min(out.data_size, size - out.data_offset);
        return out.channels > 0 && out.rate > 0;
    }

    bool to_ogg(uint8_t const* d, size_t size, info const& in, std::vector<uint8_t>& ogg) {
        if (in.mode != 15)
            return false;
        vcb_info_t const* setup = nullptr;
        for (int i = 0; i < vcb_list_count; ++i)
            if (vcb_list[i].id == in.vorbis_crc)
                setup = &vcb_list[i];
        if (setup == nullptr)
            return false;
        ogg.clear();
        ogg.reserve(in.data_size + in.data_size / 16 + 8192);
        ogg_writer w{ogg};

        // identification header: FMOD always uses 256/2048 sample blocks
        uint8_t ident[30] = {1, 'v', 'o', 'r', 'b', 'i', 's'};
        ident[11] = uint8_t(in.channels);
        uint32_t const rate = uint32_t(in.rate);
        std::memcpy(ident + 12, &rate, 4);
        ident[28] = 0x8 | (0xB << 4);
        ident[29] = 1;
        uint8_t const comment[] = {3, 'v', 'o', 'r', 'b', 'i', 's', 0, 0, 0, 0, 0, 0, 0, 0, 1};
        std::span<uint8_t const> const id_packet[] = {{ident, sizeof(ident)}};
        w.page(id_packet, 2, 0);
        std::span<uint8_t const> const headers[] = {{comment, sizeof(comment)}, {setup->codebooks, setup->size}};
        w.page(headers, 0, 0);

        // audio: "u16 size, packet" until a zero size or the end
        std::vector<std::span<uint8_t const>> page;
        uint32_t laces = 0;
        size_t page_bytes = 0;
        size_t p = in.data_offset;
        size_t const end = in.data_offset + in.data_size;
        auto const flush = [&](bool last) {
            // granule positions are only known at the end; stb_vorbis decodes
            // fine with "unknown" ones in between
            w.page(page, last ? 4 : 0, last ? int64_t(in.samples) : -1);
            page.clear();
            laces = 0;
            page_bytes = 0;
        };
        while (p + 2 <= end) {
            size_t const n = size_t(d[p]) | (size_t(d[p + 1]) << 8);
            if (n == 0 || n == 0xFFFF || p + 2 + n > end)
                break;
            if (laces + lacing_count(n) > 255 || page_bytes + n > 8192)
                flush(false);
            page.emplace_back(d + p + 2, n);
            laces += lacing_count(n);
            page_bytes += n;
            p += 2 + n;
        }
        flush(true);
        (void)size;
        return true;
    }

    bool decode(uint8_t const* d, size_t size, info& in, std::vector<float>& out) {
        out.clear();
        if (in.mode == 2) {
            size_t const n = in.data_size / 2;
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                int16_t s;
                std::memcpy(&s, d + in.data_offset + i * 2, 2);
                out[i] = float(s) / 32768.f;
            }
            return true;
        }
        if (in.mode != 15)
            return false;
        std::vector<uint8_t> ogg;
        if (!to_ogg(d, size, in, ogg))
            return false;
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(ogg.data(), int(ogg.size()), &err, nullptr);
        if (v == nullptr)
            return false;
        float buf[4096];
        int got;
        while ((got = stb_vorbis_get_samples_float_interleaved(v, in.channels, buf, 4096 / in.channels * in.channels)) > 0)
            out.insert(out.end(), buf, buf + size_t(got) * size_t(in.channels));
        stb_vorbis_close(v);
        return !out.empty();
    }
}
