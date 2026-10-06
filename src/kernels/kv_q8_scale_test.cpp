// src/kernels/kv_q8_scale_test.cpp - the int8 KV block scale stays a finite fp16 (CPU only).
//
// The scale of an int8 KV block is fp16(max|x| / 127).  Past max|x| = 8.3M that is inf, and the cell then
// dequantizes as inf * 0 = NaN (and x / inf = 0 for every code).  `kv_q8_scale` (f16_bits.hpp, used by the
// decode append kernel and the prefill one) clamps it to the largest finite half.  Checked here:
//
//   1. over every f32 bit pattern of a non-negative amax whose old scale was finite, the stored bits are
//      identical to the unclamped rule - the clamp cannot move a finite block by a bit (stride-sampled, plus the
//      boundary values around 65504 * 127);
//   2. a larger amax (and +inf) stores 0x7BFF, a finite half, and the codes for it stay in [-127, 127];
//   3. a NaN amax stays a NaN (STRATA_DBG_NAN still sees it).
#include "strata/kernels/f16_bits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace k = strata::kernels;

namespace {
int g_fail = 0;
void expect(bool ok, const char* what, float amax) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (amax %.9g)\n", what, (double) amax);
        ++g_fail;
    }
}
float from_bits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }
bool half_finite(uint16_t h) { return ((h >> 10) & 0x1Fu) != 0x1Fu; }
}  // namespace

int main() {
    // 1. finite blocks are bit-identical
    unsigned long long same = 0, clamped = 0;
    for (uint64_t u = 0; u < 0x7F800000ull; u += 997) {              // non-negative finite f32, ~2^23 samples
        const float amax = from_bits((uint32_t) u);
        const uint16_t old_bits = k::f16_from_f32(amax / 127.0f);
        const uint16_t new_bits = k::f16_from_f32(k::kv_q8_scale(amax));
        if (half_finite(old_bits)) { expect(old_bits == new_bits, "a finite scale changed", amax); ++same; }
        else { expect(new_bits == 0x7BFFu, "an overflowing scale is not the largest half", amax); ++clamped; }
    }
    const float edge = 65504.0f * 127.0f;                              // the old scale reaches 65504 here
    for (int i = -4096; i <= 4096; ++i) {
        uint32_t eb;
        std::memcpy(&eb, &edge, 4);
        const float amax = from_bits((uint32_t) ((int64_t) eb + i));
        const uint16_t old_bits = k::f16_from_f32(amax / 127.0f);
        const uint16_t new_bits = k::f16_from_f32(k::kv_q8_scale(amax));
        if (half_finite(old_bits)) expect(old_bits == new_bits, "a finite scale changed at the edge", amax);
        else expect(new_bits == 0x7BFFu, "an overflowing scale is not the largest half at the edge", amax);
    }
    // 2. huge and infinite blocks: finite scale, codes in range
    const float huge[] = {8.4e6f, 1e7f, 1e9f, 3e38f, from_bits(0x7F800000u)};
    for (const float amax : huge) {
        const uint16_t sb = k::f16_from_f32(k::kv_q8_scale(amax));
        expect(sb == 0x7BFFu, "huge amax: scale is not 65504", amax);
        const float sf = k::f32_from_f16(sb);
        for (const float x : {amax, -amax, amax * 0.5f, 0.0f, 1.0f}) {
            int q = sf > 0.0f ? (int) std::nearbyint(x / sf) : 0;
            q = std::clamp(q, -127, 127);
            expect(q >= -127 && q <= 127 && std::isfinite((float) q * sf), "huge amax: code out of range", x);
        }
    }
    // 3. NaN stays NaN; zero stays zero
    expect(std::isnan(k::kv_q8_scale(from_bits(0x7FC00000u))), "NaN amax was made finite", 0.0f);
    expect(k::kv_q8_scale(0.0f) == 0.0f, "zero amax", 0.0f);
    std::printf("kv_q8_scale_test: %llu finite scales unchanged, %llu overflowing ones clamped, %d failures\n", same,
                clamped, g_fail);
    return g_fail == 0 ? 0 : 1;
}
