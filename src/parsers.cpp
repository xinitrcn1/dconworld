#include <cctype>
#include <charconv>
#include <cstdlib>

#include "parsers.hpp"

namespace parsers {
    uint32_t parse_uint(std::string_view contents, error_handler& err) noexcept {
        int32_t const num = parse_int(contents, err);
        if (num < 0)
            err.add_error("expected unsigned got " + std::to_string(num));
        return uint32_t(num);
    }
    int32_t parse_int(std::string_view contents, error_handler& err) noexcept {
        if (!contents.empty() && contents[0] == '+')
            contents.remove_prefix(1);
        int32_t v = 0;
        auto const [ptr, ec] = std::from_chars(contents.data(), contents.data() + contents.size(), v);
        if (ec != std::errc{})
            err.add_error("expected integer got \"" + std::string{contents} + "\"");
        return v;
    }
    float parse_float(std::string_view contents, error_handler& err) noexcept {
        if (!contents.empty() && contents[0] == '+')
            contents.remove_prefix(1);
        //float v = 0.f;
        //auto const [ptr, ec] = std::from_chars(contents.data(), contents.data() + contents.size(), v);
        //if (ec != std::errc{})
        //    err.add_error("expected number got \"" + std::string{contents} + "\"");
        //return v;
        std::string token(contents.data(), contents.data() + contents.size());
        char* end = nullptr;
        float val = std::strtof(token.c_str(), &end);
        if (end != token.c_str() + token.size()) {
            err.add_error("expected number got \"" + std::string{contents} + "\"");
        }
        return val;
    }
    bool parse_bool(std::string_view contents, error_handler& err) noexcept {
        // settings files use ON/OFF, properties use true/false
        return contents == "ON" || contents == "true" || contents == "True" || contents == "1";
    }
    std::string parse_string(std::string_view contents, error_handler& err) noexcept {
        return std::string{contents};
    }

    key_value_list parse_key_value_list(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept {
        key_value_list o;
        auto const end = gen.tokens.cend();
        if (it != end && it->type == token::type::identifier) {
            o.key = std::string{it->to_string_view(gen)};
            ++it;
        } else {
            err.add_error("expected key identifier");
        }
        if (it != end && it->type == token::type::semicolon)
            ++it;
        else
            err.add_error("expected semicolon after key");
        o.values = parse_string_list(gen, err, it);
        return o;
    }
    string_list parse_string_list(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept {
        string_list o;
        auto const end = gen.tokens.cend();
        while (it != end && it->type != token::type::newline) {
            if (it->type == token::type::identifier)
                o.emplace_back(it->to_string_view(gen));
            else if (it->type != token::type::comma)
                err.add_error("identifier/comma expected, got \"" + std::string{it->to_string_view(gen)} + "\"");
            ++it;
        }
        return o;
    }
    color_3f parse_color(token_generator& gen, error_handler& err, token_generator::const_iterator& it) noexcept {
        color_3f o;
        float* const comps[3] = {&o.r, &o.g, &o.b};
        auto const end = gen.tokens.cend();
        for (uint32_t i = 0; i < 3; ++i) {
            if (i > 0) {
                if (it == end || it->type != token::type::comma) {
                    err.add_error("expected comma in colour");
                    return o;
                }
                ++it;
            }
            if (it == end || it->type != token::type::identifier) {
                err.add_error("expected colour component");
                return o;
            }
            *comps[i] = parse_float(it->to_string_view(gen), err);
            ++it;
        }
        return o;
    }

    void error_handler::add_error(std::string_view sv) {
        accumulated_errors += filename + ':' + std::to_string(line) + ": " + std::string{sv} + '\n';
        ++error_count;
    }
    void error_handler::add_warning(std::string_view sv) {
        accumulated_warnings += filename + ':' + std::to_string(line) + ": " + std::string{sv} + '\n';
        ++warning_count;
    }
    void error_handler::add_standalone_error(std::string_view sv) {
        accumulated_errors += "<builtin>: " + std::string{sv} + '\n';
        ++error_count;
    }
    void error_handler::add_standalone_warning(std::string_view sv) {
        accumulated_warnings += "<builtin>: " + std::string{sv} + '\n';
        ++warning_count;
    }

    std::string_view token::to_string_view(token_generator const& gen) const noexcept {
        return gen.view.substr(pos, len);
    }

    static bool is_identifier_start(char ch) noexcept {
        return isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '.' || ch == '!' || ch == '_' || ch == '+';
    }
    static bool is_identifier_stopper(char ch) noexcept {
        return ch == '\n' || ch == '\r' || ch == ',';
    }

    // Identifiers run until a newline/comma or a ": " separator, so multi-word
    // keys ("Room Setting Templates") and rich values ("A><B><C") are single tokens
    token_generator::token_generator(std::string_view sv) : view{sv} {
        uint32_t line = 1;
        size_t i = 0;
        size_t const n = sv.size();
        tokens.reserve(n / 8);
        while (i < n) {
            char const ch = sv[i];
            if (ch == '\n') {
                tokens.push_back(token{uint32_t(i), 1, line, token::type::newline});
                ++line;
                ++i;
            } else if (ch == '/' && i + 1 < n && sv[i + 1] == '/') {
                while (i < n && sv[i] != '\n')
                    ++i;
            } else if (ch == '{' || ch == '}' || ch == '(' || ch == ')' || ch == ',' || ch == ':') {
                auto const t = ch == '{' ? token::type::lbrace
                    : ch == '}' ? token::type::rbrace
                    : ch == '(' ? token::type::lparen
                    : ch == ')' ? token::type::rparen
                    : ch == ',' ? token::type::comma
                    : token::type::semicolon;
                tokens.push_back(token{uint32_t(i), 1, line, t});
                ++i;
            } else if (is_identifier_start(ch)) {
                size_t const start = i;
                do {
                    ++i;
                    if (i + 1 < n && sv[i] == ':' && sv[i + 1] == ' ')
                        break;
                } while (i < n && !is_identifier_stopper(sv[i]));
                size_t end = i;
                while (end > start && (sv[end - 1] == ' ' || sv[end - 1] == '\t'))
                    --end;
                tokens.push_back(token{uint32_t(start), uint32_t(end - start), line, token::type::identifier});
            } else {
                ++i;
            }
        }
        cur_tok = tokens.cbegin();
    }

    void region_properties::subregion(string_list&& sl, error_handler& err, region_context& ctx) noexcept {
        for (auto& s : sl)
            subregions.push_back(std::move(s));
    }

    void room_template::fade_palette(string_list&& sl, error_handler& err, room_context& ctx) noexcept {
        // "FadePalette: <palette>, <amount>"
        if (sl.size() >= 1)
            fade_palette_index = parse_uint(sl[0], err);
        if (sl.size() >= 2)
            fade_palette_amount = parse_float(sl[1], err);
    }

    void room_template::apply(room_template const& o) noexcept {
        auto const set = [](auto& dst, auto const& src) {
            if (src.has_value())
                dst = src;
        };
        set(danger_type, o.danger_type);
        set(palette, o.palette);
        set(effect_color_a, o.effect_color_a);
        set(effect_color_b, o.effect_color_b);
        set(fade_palette_index, o.fade_palette_index);
        set(fade_palette_amount, o.fade_palette_amount);
        set(grime, o.grime);
        set(clouds, o.clouds);
        set(ceiling_drips, o.ceiling_drips);
        set(wave_speed, o.wave_speed);
        set(wave_length, o.wave_length);
        set(wave_amplitude, o.wave_amplitude);
        set(second_wave_length, o.second_wave_length);
        set(second_wave_amplitude, o.second_wave_amplitude);
        set(water_reflection_alpha, o.water_reflection_alpha);
        set(bkg_drone_volume, o.bkg_drone_volume);
        set(bkg_drone_no_threat_volume, o.bkg_drone_no_threat_volume);
        set(random_item_density, o.random_item_density);
        set(random_item_spear_chance, o.random_item_spear_chance);
        set(rumble_intensity, o.rumble_intensity);
        set(rain_intensity, o.rain_intensity);
        set(room_specific_script_active, o.room_specific_script_active);
        set(wet_terrain, o.wet_terrain);
    }
}
