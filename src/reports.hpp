#pragma once

#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace reports {
    inline void add_log(std::string_view sv) {
        std::fwrite(sv.data(), 1, sv.size(), stdout);
        std::fputc('\n', stdout);
    }

    [[noreturn]] inline void error_fatal(std::string_view sv) {
        add_log(sv);
        std::fflush(stdout);
        std::abort();
    }
}
