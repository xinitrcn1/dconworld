#pragma once

// rw::keyed (math.hpp) in dcon's vectors: the stateless generator's rolls
// for ve::vector_size objects at once, lane for lane the bits the scalar
// computes (selftest checks it). A whole-world pass keys each lane by its
// object (key(seed, tick, ids, salt), `ids` the lanes' dcon ids' .value or
// seeds), then takes draw(keys, n) for its n-th roll
#include <cstdint>

#include "dcon_generated.hpp"
#include "math.hpp"

namespace keyed_random {
    // ve::int_vector has no shifts; the logical right shift per target
    template<int N>
    inline ve::int_vector shift_right(ve::int_vector v) noexcept {
#if defined(__AVX512BW__)
        return ve::int_vector{_mm512_srli_epi32(v.value, N)};
#elif defined(__AVX2__)
        return ve::int_vector{_mm256_srli_epi32(v.value, N)};
#else
        // (SSE, and ve_avx's int_vector, are 128 bits wide)
        return ve::int_vector{_mm_srli_epi32(v.value, N)};
#endif
    }
    inline ve::int_vector mix(ve::int_vector x) noexcept {
        x = x ^ shift_right<16>(x);
        x = x * ve::int_vector{int32_t(0x7FEB352Du)};
        x = x ^ shift_right<15>(x);
        x = x * ve::int_vector{int32_t(0x846CA68Bu)};
        x = x ^ shift_right<16>(x);
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
        return ve::to_float(shift_right<8>(bits)) * (1.f / 16777215.f);
    }
}
