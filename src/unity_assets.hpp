#pragma once

// Reading the game's built-in Unity data (RainWorld_Data: resources.assets
// and its .resS stream) at runtime: the Futile atlases' textures and their
// TexturePacker sheets. Nothing of it is bundled; without the folder the
// renderer falls back to plain shapes.
//
// Only Unity 2020.3's serialized file layout (version 22, no type trees) is
// read, which is what the game ships.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace unity {
    // Texture2D decoded to RGBA8, top row first
    struct image {
        std::string name;
        int32_t width = 0;
        int32_t height = 0;
        std::vector<uint8_t> rgba;
    };

    struct asset_library {
        std::string dir;
        // resources.assets
        std::vector<uint8_t> data;
        struct object {
            int32_t cls;
            uint64_t offset;
            uint32_t size;
        };
        std::vector<object> objects;

        // opens `dir`/resources.assets; false when it isn't there or can't be read
        bool open(std::string const& dir);
        bool texture(std::string_view name, image& out) const;
        bool text(std::string_view name, std::string& out) const;
    };

    // where the Unity data is: an explicit folder (--unity, RW_UNITY), else
    // next to one of the StreamingAssets roots, else ./unity-assets/staging
    std::string find_asset_dir(std::vector<std::string> const& roots, std::string const& explicit_dir);
}
