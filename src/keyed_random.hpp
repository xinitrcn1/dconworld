#pragma once

// rw::keyed (math.hpp) in dcon's vectors: the stateless generator's rolls
// for ve::vector_size objects at once, lane for lane the bits the scalar
// computes (selftest checks it). A whole-world pass keys each lane by its
// object (key(seed, tick, ids, salt), `ids` the lanes' dcon ids' .value or
// seeds), then takes draw(keys, n) for its n-th roll
#include <cstdint>

#include "dcon_generated.hpp"
#include "math.hpp"
#include "ve_utils.hpp"

namespace keyed_random {
    inline ve::int_vector mix(ve::int_vector x) noexcept {
        x = x ^ ve::shift_right<16>(x);
        x = x * ve::int_vector{int32_t(0x7FEB352Du)};
        x = x ^ ve::shift_right<15>(x);
        x = x * ve::int_vector{int32_t(0x846CA68Bu)};
        x = x ^ ve::shift_right<16>(x);
        return x;
    }
    // rw::keyed::key for each lane's id
    inline ve::int_vector key(uint32_t seed, uint32_t tick, ve::int_vector ids, uint32_t salt) noexcept {
        ve::int_vector const fixed{int32_t(seed ^ tick * rw::keyed::k_tick ^ salt * rw::keyed::k_salt)};
        return fixed ^ ids * ve::int_vector{int32_t(rw::keyed::k_id)};
    }
    // rw::keyed::draw: the n-th roll of each lane's key
    inline ve::int_vector draw(ve::int_vector keys, uint32_t n) noexcept {
        return mix(keys ^ ve::int_vector{int32_t(n * rw::keyed::k_draw)});
    }
    // rw::keyed::to_value: [0, 1]
    inline ve::fp_vector to_value(ve::int_vector bits) noexcept {
        return ve::to_float(ve::shift_right<8>(bits)) * (1.f / 16777215.f);
    }
}
