#include <algorithm>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "layered_fs.hpp"
#include "config.h"
#include "text.hpp"

namespace layered_fs {
    mapped_file::mapped_file(mapped_file&& o) noexcept : base{o.base}, size{o.size} {
        o.base = nullptr;
        o.size = 0;
    }
    mapped_file& mapped_file::operator=(mapped_file&& o) noexcept {
        if (this != &o) {
            if (base != nullptr)
                ::munmap(base, size);
            base = o.base;
            size = o.size;
            o.base = nullptr;
            o.size = 0;
        }
        return *this;
    }
    mapped_file::~mapped_file() noexcept {
        if (base != nullptr)
            ::munmap(base, size);
    }

    static std::string_view basename_of(std::string_view path) noexcept {
        if (auto const pos = path.find_last_of(NATIVE_SEPARATOR); pos != std::string_view::npos)
            return path.substr(pos + 1);
        return path;
    }

    std::optional<mapped_file> file::open_file(filesystem const& fs) const noexcept {
        auto const lfp = text::to_lowercase_native(path);
        for (auto const& rt : fs.roots) {
            auto const it = rt.files.find(lfp);
            if (it == rt.files.end())
                continue;
            int const fd = ::open((rt.root + it->second).c_str(), O_RDONLY);
            if (fd < 0)
                continue;
            mapped_file mf;
            struct stat st;
            if (::fstat(fd, &st) == 0 && st.st_size > 0) {
                void* p = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
                if (p != MAP_FAILED) {
                    mf.base = p;
                    mf.size = size_t(st.st_size);
                }
            }
            ::close(fd);
            // empty files are valid (size 0, no mapping)
            return std::optional<mapped_file>{std::move(mf)};
        }
        return std::nullopt;
    }
    std::string file::get_basename() const noexcept {
        return text::to_lowercase_native(basename_of(path));
    }
    std::string file::get_filename() const noexcept {
        auto const s = get_basename();
        return s.substr(0, s.find_first_of('.'));
    }
    std::string file::get_extension() const noexcept {
        auto const s = get_basename();
        if (auto const pos = s.find_first_of('.'); pos != std::string::npos)
            return s.substr(pos + 1);
        return {};
    }

    template<typename T, typename F>
    static std::vector<T> merge_layers(filesystem const& fs, std::string const& path, F&& get_map) {
        auto const ldir = text::to_lowercase_native(path);
        std::vector<T> result;
        for (auto const& rt : fs.roots) {
            auto const& m = get_map(rt);
            if (auto const it = m.find(ldir); it != m.end())
                for (auto const& child : it->second)
                    result.push_back(T{child});
        }
        // stable so that the first (highest priority) layer survives dedup
        std::stable_sort(result.begin(), result.end(), [](T const& a, T const& b) { return a.path < b.path; });
        result.erase(std::unique(result.begin(), result.end(), [](T const& a, T const& b) { return a.path == b.path; }), result.end());
        return result;
    }

    std::vector<file> directory::get_files(filesystem const& fs) const noexcept {
        return merge_layers<file>(fs, path, [](filesystem_root const& rt) -> auto const& { return rt.dir_files; });
    }
    std::vector<directory> directory::get_subdirs(filesystem const& fs) const noexcept {
        return merge_layers<directory>(fs, path, [](filesystem_root const& rt) -> auto const& { return rt.dir_subdirs; });
    }
    file directory::get_single_file(std::string_view sv) const noexcept {
        return file{path + NATIVE_SEPARATOR + std::string{sv}};
    }
    directory directory::get_single_subdir(std::string_view sv) const noexcept {
        return directory{path + NATIVE_SEPARATOR + std::string{sv}};
    }
    std::string directory::get_basename() const noexcept {
        return text::to_lowercase_native(basename_of(path));
    }

    static void generate_tree_1(filesystem_root& rt, std::string const& rel_dir) {
        DIR* dp = ::opendir((rt.root + rel_dir).c_str());
        if (dp == nullptr)
            return;
        auto const ldir = text::to_lowercase_native(rel_dir);
        auto& files = rt.dir_files[ldir];
        auto& subdirs = rt.dir_subdirs[ldir];
        while (struct dirent* entry = ::readdir(dp)) {
            std::string const name{entry->d_name};
            if (name[0] == '.')
                continue;
            auto const rel = rel_dir + NATIVE_SEPARATOR + name;
            auto type = entry->d_type;
            if (type == DT_UNKNOWN || type == DT_LNK) {
                struct stat st;
                if (::stat((rt.root + rel).c_str(), &st) != 0)
                    continue;
                type = S_ISDIR(st.st_mode) ? DT_DIR : (S_ISREG(st.st_mode) ? DT_REG : DT_UNKNOWN);
            }
            if (type == DT_REG) {
                auto lrel = text::to_lowercase_native(rel);
                rt.files.insert_or_assign(lrel, rel);
                files.push_back(std::move(lrel));
            } else if (type == DT_DIR) {
                // mods are separate layers, they get registered as their own roots
                auto const lname = text::to_lowercase_native(name);
                if (lname == "mods" || lname == "mergedmods" || lname == "modify")
                    continue;
                subdirs.push_back(text::to_lowercase_native(rel));
                generate_tree_1(rt, rel);
            }
        }
        ::closedir(dp);
    }

    void filesystem::add_root(std::string path) {
        while (!path.empty() && (path.back() == '/' || path.back() == '\\'))
            path.pop_back();
        filesystem_root rt;
        rt.root = std::move(path);
        roots.push_back(std::move(rt));
    }

    void filesystem::generate_unpopulated_tree() {
        for (auto& rt : roots) {
            if (!rt.indexed) {
                generate_tree_1(rt, "");
                rt.indexed = true;
            }
        }
    }
}
