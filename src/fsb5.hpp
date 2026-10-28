#pragma once

// FMOD sound banks (FSB5) as stored in Unity AudioClips: PCM16, or Vorbis
// with the setup header replaced by its CRC32 (restored from vgmstream's
// table of FMOD's setup headers). Vorbis samples are rewrapped as an Ogg
// Vorbis stream so stb_vorbis can decode them.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fsb5 {
    struct info {
        int channels = 0;
        int rate = 0;
        uint32_t samples = 0;
        uint32_t mode = 0; // 2 PCM16, 15 Vorbis
        size_t data_offset = 0;
        size_t data_size = 0;
        uint32_t vorbis_crc = 0;
    };

    // header of the first sample of the bank
    bool parse(uint8_t const* data, size_t size, info& out);
    // a Vorbis sample as a complete Ogg Vorbis file
    bool to_ogg(uint8_t const* data, size_t size, info const& in, std::vector<uint8_t>& ogg);
    // the whole sample, interleaved float
    bool decode(uint8_t const* data, size_t size, info& in, std::vector<float>& out);
}
