// src/kernels/cpu/act_quant_oracle_test.cpp - the activation quantizer honours the oracle flag on every dispatch (CPU only).
//
// `expert_set_oracle_q8_0(true)` selects ggml's x86 Q8_0 quantization (scale = fp16(amax / 127), code =
// round(x * (127 / amax))).  On AVX-512 `act_quant_q8_1` does that; the AVX2 quantizer (`act_quant_q8_1_avx2`, which
// `act_quant_any` picks on a CPU without AVX-512 and which feeds the native Q2_0 down projection) ignored the flag
// and kept its own rule, so a CPU without AVX-512 never ran the exact contract.  Checked against a scalar
// transcription of both rules, with the flag on and off:
//
//   * flag on:  act_quant_q8_1_avx2 and act_quant_any equal the oracle transcription (codes, fp16 scale, sum, hx);
//   * flag off: act_quant_q8_1_avx2 equals the scalar rule it always had (so the default path is unchanged);
//   * the two rules differ on this data (the flag does something);
//   * on an AVX-512 CPU, act_quant_q8_1 under the flag equals the transcription too.
#include "strata/kernels/cpu/expert.hpp"
#include "strata/kernels/cpu/expert_layout.hpp"
#include "strata/kernels/f16_bits.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace c = strata::kernels::cpu;
namespace k = strata::kernels;

namespace {
int g_fail = 0;

// ggml's x86 quantize_row_q8_0 rule
void ref_oracle(const float* x, int n, c::ActQ& a) {
    a.nchunks = n / c::QKA;
    for (int ch = 0; ch < a.nchunks; ++ch) {
        const float* xb = x + ch * c::QKA;
        float amax = 0.f;
        for (int j = 0; j < c::QKA; ++j) amax = std::fmax(amax, std::fabs(xb[j]));
        const float inv = amax != 0.f ? 127.0f / amax : 0.f;
        int32_t sum = 0;
        for (int j = 0; j < c::QKA; ++j) {
            const int q = (int) std::nearbyint(xb[j] * inv);
            a.q[ch * c::QKA + j] = (int8_t) std::clamp(q, -128, 127);
            sum += a.q[ch * c::QKA + j];
        }
        a.scale[ch] = k::f32_from_f16(k::f16_from_f32(amax / 127.0f));
        a.sum[ch] = sum;
        a.hx[ch] = a.scale[ch] * (float) sum;
    }
}

// the AVX2 quantizer's own rule (what the default path did before and still does)
void ref_default(const float* x, int n, c::ActQ& a) {
    a.nchunks = n / c::QKA;
    for (int ch = 0; ch < a.nchunks; ++ch) {
        const float* xb = x + ch * c::QKA;
        float amax = 0.f;
        for (int j = 0; j < c::QKA; ++j) amax = std::fmax(amax, std::fabs(xb[j]));
        const float s = amax > 0.f ? amax / 127.f : 0.f;
        const float inv = s > 0.f ? 1.f / s : 0.f;
        int32_t sum = 0;
        for (int j = 0; j < c::QKA; ++j) {
            const float t = xb[j] * inv;
            const int q = std::clamp((int) (t + (t >= 0.f ? 0.5f : -0.5f)), -127, 127);
            a.q[ch * c::QKA + j] = (int8_t) q;
            sum += q;
        }
        a.scale[ch] = s;
        a.sum[ch] = sum;
        a.hx[ch] = s * (float) sum;
    }
}

bool same(const c::ActQ& a, const c::ActQ& b, int n) {
    if (a.nchunks != b.nchunks) return false;
    const int nc = n / c::QKA;
    return std::memcmp(a.q, b.q, (size_t) n) == 0 && std::memcmp(a.scale, b.scale, nc * 4) == 0 &&
           std::memcmp(a.sum, b.sum, nc * 4) == 0 && std::memcmp(a.hx, b.hx, nc * 4) == 0;
}

void check(bool ok, const char* what, int trial) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s (trial %d)\n", what, trial); ++g_fail; }
}
}  // namespace

int main() {
    if (!c::cpu_avx2_ok()) { std::printf("act_quant_oracle_test: no AVX2 here, skipped\n"); return 0; }
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    static c::ActQ got, want, other;
    const int n = c::H;
    std::vector<float> x((size_t) n);
    int differs = 0;
    for (int trial = 0; trial < 200; ++trial) {
        for (int i = 0; i < n; ++i) {
            const int ch = i / c::QKA;
            const float scale = (ch % 5 == 0) ? 1e-3f : (ch % 7 == 0 ? 300.f : 1.f);
            x[(size_t) i] = nd(rng) * scale;
        }
        if (trial % 3 == 0) std::fill(x.begin(), x.begin() + c::QKA, 0.f);            // an all-zero chunk
        ref_oracle(x.data(), n, want);

        c::expert_set_oracle_q8_0(true);
        std::memset(&got, 0xA5, sizeof got);
        c::act_quant_q8_1_avx2(x.data(), n, got);
        check(same(got, want, n), "act_quant_q8_1_avx2 under the oracle flag differs from the oracle rule", trial);
        std::memset(&got, 0xA5, sizeof got);
        c::act_quant_any(x.data(), n, got);
        check(same(got, want, n), "act_quant_any under the oracle flag differs from the oracle rule", trial);
        if (c::cpu_avx512_ok()) {
            std::memset(&got, 0xA5, sizeof got);
            c::act_quant_q8_1(x.data(), n, got);
            check(same(got, want, n), "act_quant_q8_1 under the oracle flag differs from the oracle rule", trial);
        }

        c::expert_set_oracle_q8_0(false);
        ref_default(x.data(), n, other);
        std::memset(&got, 0xA5, sizeof got);
        c::act_quant_q8_1_avx2(x.data(), n, got);
        check(same(got, other, n), "act_quant_q8_1_avx2 without the flag changed", trial);
        if (!same(want, other, n)) ++differs;
    }
    c::expert_set_oracle_q8_0(false);
    check(differs > 0, "the two rules never differ on this data, the test shows nothing", -1);
    std::printf("act_quant_oracle_test: %d failures (rules differ on %d/200 vectors)\n", g_fail, differs);
    return g_fail == 0 ? 0 : 1;
}
