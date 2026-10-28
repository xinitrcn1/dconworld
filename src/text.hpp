#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace text {
    inline std::string to_lowercase_native(std::string s) noexcept {
        std::transform(s.begin(), s.end(), s.begin(), [](char ch) {
            return char(tolower(static_cast<unsigned char>(ch)));
        });
        return s;
    }
    inline std::string to_lowercase_native(std::string_view sv) noexcept {
        return to_lowercase_native(std::string{sv});
    }
}
