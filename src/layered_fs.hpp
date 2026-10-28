#pragma once

// Layered, case-insensitive, read-only view over the game's StreamingAssets
// (plus optional mod roots). The whole game is treated as a RomFS: the
// directory tree is indexed once at startup and files are mmap'd on demand.
//
// Paths handed around are "relative" and always start with a separator, e.g.
// "/world/su-rooms/su_a22.txt". Lookups are case insensitive. Roots earlier in
// `filesystem::roots` take priority over later ones (mergedmods > base game).

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace layered_fs {
    struct filesystem;

    // Read-only memory mapping of a file, unmapped on destruction
    struct mapped_file {
        mapped_file() noexcept = default;
        mapped_file(mapped_file const&) = delete;
        mapped_file& operator=(mapped_file const&) = delete;
        mapped_file(mapped_file&& o) noexcept;
        mapped_file& operator=(mapped_file&& o) noexcept;
        ~mapped_file() noexcept;

        std::string_view to_string_view() const noexcept {
            return std::string_view{static_cast<char const*>(base), size};
        }

        void* base = nullptr;
        size_t size = 0;
    };

    struct file {
        std::optional<mapped_file> open_file(filesystem const& fs) const noexcept;
        // lowercase "name.ext"
        std::string get_basename() const noexcept;
        // lowercase "name" (up to the first dot)
        std::string get_filename() const noexcept;
        // lowercase "ext" (after the first dot)
        std::string get_extension() const noexcept;

        std::string path;
    };

    struct directory {
        // Direct children only, deduplicated across layers (higher layers win)
        std::vector<file> get_files(filesystem const& fs) const noexcept;
        std::vector<directory> get_subdirs(filesystem const& fs) const noexcept;
        file get_single_file(std::string_view sv) const noexcept;
        directory get_single_subdir(std::string_view sv) const noexcept;
        std::string get_basename() const noexcept;

        std::string path;
    };

    struct filesystem_root {
        std::string root;
        // lowercase relative path -> on-disk relative path
        std::unordered_map<std::string, std::string> files;
        // lowercase relative dir -> lowercase relative paths of direct children
        std::unordered_map<std::string, std::vector<std::string>> dir_files;
        std::unordered_map<std::string, std::vector<std::string>> dir_subdirs;
        bool indexed = false;
    };

    struct filesystem {
        // Index every root that has not been indexed yet
        void generate_unpopulated_tree();
        void add_root(std::string path);
        directory get_root() const { return directory{""}; }

        std::vector<filesystem_root> roots;
    };
}
