#pragma once

// Just enough of Unity's AssetBundle format (UnityFS, version 6+, LZ4 blocks)
// to find the AudioClips of the game's sound bundles
// (StreamingAssets/AssetBundles/loadedsoundeffects, music_songs) and read
// their FSB5 data. The loose files the game ships next to them are empty
// placeholders.

#include <cstdint>
#include <string>
#include <vector>

#include "layered_fs.hpp"

namespace unity {
    // AudioClip (class 83): its FSB5 data is `size` bytes at `offset` of the
    // bundle's resource node
    struct audio_clip {
        std::string name;
        int32_t channels = 0;
        int32_t frequency = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        int32_t compression = 0; // AudioCompressionFormat: 0 PCM, 1 Vorbis...
    };

    struct bundle {
        struct block {
            uint32_t usize, csize;
            uint16_t flags;
            uint64_t uoff, coff;
        };
        struct node {
            uint64_t offset, size;
            uint32_t flags;
            std::string name;
        };

        layered_fs::mapped_file file;
        std::vector<block> blocks;
        std::vector<node> nodes;
        // last decompressed block
        mutable int64_t cached = -1;
        mutable std::vector<uint8_t> cache;

        // parses the header and block/node tables
        bool open(layered_fs::mapped_file&& f);
        // reads `size` bytes at `offset` of the uncompressed data
        bool read(uint64_t offset, uint64_t size, std::vector<uint8_t>& out) const;
    };

    // Every AudioClip of the bundle's serialized file; `resource_base` is the
    // offset of the resource node their data lives in
    bool audio_clips(bundle const& b, std::vector<audio_clip>& out, uint64_t& resource_base);
}
