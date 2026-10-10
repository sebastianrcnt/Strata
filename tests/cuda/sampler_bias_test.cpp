// tests/cuda/sampler_bias_test.cpp - the per-request logit bias (sampler.hpp: sampler_set_request_bias).
//
// What it pins: the bias is added after the --ban-tokens base and a banned token stays -inf even at +100; -100 is a
// ban; a CUDA graph captured BEFORE any request sees each request's entries on replay (the count lives in the fixed
// buffer, not in a kernel argument) and, once cleared, replays bit-identical logits to a run that never had a bias
// (request A with a bias, then request B without = the no-bias baseline); the draft head's subset rows get the same
// bias (a token listed twice in the subset on both rows); bad input is refused and leaves the previous state.
#include "strata/kernels/sampler.hpp"
#include "strata/kernels/verify_kernels.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static void check(cudaError_t e) {
    if (e != cudaSuccess) { std::fprintf(stderr, "%s\n", cudaGetErrorString(e)); std::exit(1); }
}
static void require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
    std::printf("ok: %s\n", what);
}
template<class T> static T* upload(const std::vector<T>& v) {
    T* d = nullptr;
    check(cudaMalloc((void**) &d, v.size() * sizeof(T)));
    check(cudaMemcpy(d, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice));
    return d;
}

using namespace strata::kernels;

namespace {
constexpr int NV = 16, T = 2;

// One captured "verify head": bans (+ the target bias), then the greedy pick - captured once, replayed per request.
struct Head {
    cudaStream_t s = nullptr;
    cudaGraphExec_t exec = nullptr;
    float* d_l = nullptr;
    int32_t* d_o = nullptr;
    std::vector<float> base;
    void init(const std::vector<float>& logits, const int32_t* rows, int n_rows, int nv, BiasRows set) {
        base = logits;
        d_l = upload(logits);
        d_o = upload(std::vector<int32_t>(T, -1));
        check(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
        SamplerParams greedy;
        greedy.greedy = true;
        cudaGraph_t g;
        check(cudaStreamBeginCapture(s, cudaStreamCaptureModeGlobal));
        if (set == BiasRows::Target && rows == nullptr) apply_bans(d_l, T, nv, s);
        else apply_bans_rows(d_l, T, nv, rows, n_rows, s, set);
        sample_tokens(d_l, T, nv, nullptr, 0, greedy, d_o, s);
        check(cudaStreamEndCapture(s, &g));
        check(cudaGraphInstantiate(&exec, g, nullptr, nullptr, 0));
        check(cudaGraphDestroy(g));
    }
    // a replay on fresh logits -> (the logits after the mask, the picks)
    std::vector<float> run(std::vector<int32_t>& picks) {
        check(cudaMemcpyAsync(d_l, base.data(), base.size() * sizeof(float), cudaMemcpyHostToDevice, s));
        check(cudaGraphLaunch(exec, s));
        check(cudaStreamSynchronize(s));
        std::vector<float> out(base.size());
        picks.assign(T, -1);
        check(cudaMemcpy(out.data(), d_l, out.size() * sizeof(float), cudaMemcpyDeviceToHost));
        check(cudaMemcpy(picks.data(), d_o, T * sizeof(int32_t), cudaMemcpyDeviceToHost));
        return out;
    }
};

bool set(std::vector<int32_t> ids, std::vector<float> vals) {
    std::string err;
    const bool ok = sampler_set_request_bias(ids.data(), vals.data(), (int) ids.size(), err);
    if (!ok) std::printf("   (refused: %s)\n", err.c_str());
    return ok;
}
bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}
}  // namespace

int main() {
    const float NEG_INF = -std::numeric_limits<float>::infinity();
    // before the buffer exists a bias is refused, and a request without one is still fine
    require(!set({1}, {5.f}), "a bias before sampler_init_request_bias is refused");
    require(set({}, {}), "an empty bias needs no buffer");

    const int bans[]{3, 7};
    sampler_set_bans(bans, 2);
    // the draft head's subset: rows -> token ids (9 is listed twice: rows 1 and 4); 3 is banned (row 3)
    const std::vector<int32_t> sub{5, 9, 1, 3, 9, 2};
    sampler_init_request_bias();
    sampler_set_draft_subset(sub.data(), (int) sub.size());

    // ---- the target head: graph captured before any request
    std::vector<float> logits(T * NV);
    for (int t = 0; t < T; ++t)
        for (int v = 0; v < NV; ++v) logits[t * NV + v] = (float) ((v * 7 + t * 3) % 11) - 0.25f * (float) v;
    logits[0 * NV + 3] = 90.f;   // banned: never picked
    logits[0 * NV + 5] = 50.f;   // row 0's best allowed token
    logits[1 * NV + 9] = 60.f;   // row 1's best allowed token
    logits[1 * NV + 11] = -0.0f; // adding nothing keeps a -0.0 a -0.0
    Head h;
    h.init(logits, nullptr, 0, NV, BiasRows::Target);
    std::vector<int32_t> picks;
    const std::vector<float> baseline = h.run(picks);
    require(picks[0] == 5 && picks[1] == 9, "no bias: the bans alone decide (baseline)");
    require(baseline[3] == NEG_INF && baseline[NV + 7] == NEG_INF, "the base bans are -inf");

    // ---- request A: a bias
    require(set({2, 5, 3, 9, 12, 12}, {60.f, -100.f, 100.f, -100.f, 1.f, 30.f}), "request A's bias is accepted");
    require(sampler_request_bias_entries() == 4, "banned ids are dropped, a repeated id is one entry");
    std::vector<float> a = h.run(picks);
    require(a[5] == NEG_INF && a[NV + 9] == NEG_INF, "-100 is a hard ban (-inf), on every row");
    require(a[3] == NEG_INF && a[NV + 3] == NEG_INF, "+100 on a banned token: still -inf (no un-banning)");
    require(a[2] == baseline[2] + 60.f && a[NV + 2] == baseline[NV + 2] + 60.f, "a value is added to the logit");
    require(a[12] == baseline[12] + 30.f, "a repeated id: the last value wins");
    bool rest = true;
    for (int t = 0; t < T; ++t)
        for (int v = 0; v < NV; ++v)
            if (v != 2 && v != 5 && v != 9 && v != 12) rest = rest && std::memcmp(&a[t * NV + v], &baseline[t * NV + v], 4) == 0;
    require(rest, "every other logit is untouched, bit for bit");
    require(picks[0] == 2 && picks[1] == 2, "the greedy pick follows the bias (graph captured before the request)");

    // ---- request B: none (A's scope cleared it) = the baseline, bit for bit
    sampler_clear_request_bias();
    require(sampler_request_bias_entries() == 0 && sampler_request_bias_draft_entries() == 0, "cleared: no entries");
    std::vector<float> b = h.run(picks);
    require(same_bits(b, baseline) && picks[0] == 5 && picks[1] == 9,
            "request B without a bias after A replays the no-bias baseline bit-identically");
    require(std::signbit(b[NV + 11]), "-0.0 stays -0.0 (no add when there is no bias)");

    // ---- +100 all but forces a token; bad input is refused and leaves the state as it was
    require(set({14}, {100.f}), "+100 accepted");
    std::vector<float> c = h.run(picks);
    require(picks[0] == 14 && picks[1] == 14, "+100 wins over every ordinary logit");
    const float nanv = std::nanf("");
    require(!set({1}, {100.5f}) && !set({1}, {-101.f}) && !set({1}, {nanv}) && !set({-1}, {1.f}),
            "out-of-range values, NaN and negative ids are refused");
    std::vector<int32_t> many(kMaxRequestBias + 1, 1);
    std::vector<float> manyv(kMaxRequestBias + 1, 1.f);
    require(!set(many, manyv), "more than kMaxRequestBias entries are refused");
    require(same_bits(h.run(picks), c), "a refused request leaves the previous entries in place");
    require(set({4, 6}, {0.f, 0.f}) && sampler_request_bias_entries() == 0, "0 values are no-ops (dropped)");
    require(same_bits(h.run(picks), baseline), "... and replay the baseline");

    // ---- the draft head over the subset: its rows, with the same bias
    std::vector<float> dl(T * (int) sub.size());
    for (int t = 0; t < T; ++t)
        for (int r = 0; r < (int) sub.size(); ++r) dl[t * sub.size() + r] = (float) (r + t);
    const std::vector<int32_t> rows = sampler_subset_bans(sub.data(), (int) sub.size());
    require(rows == std::vector<int32_t>({3}), "draft ban rows: token 3 at row 3");
    int32_t* d_rows = upload(rows);
    Head d;
    d.init(dl, d_rows, (int) rows.size(), (int) sub.size(), BiasRows::Draft);
    const std::vector<float> dbase = d.run(picks);
    require(picks[0] == 5 && picks[1] == 5, "draft, no bias: row 5 (token 2) wins, row 3 banned");
    require(set({9, 2, 3, 40}, {50.f, -100.f, 100.f, 7.f}), "a bias with draft rows");
    require(sampler_request_bias_draft_entries() == 3, "token 9 -> rows 1 and 4, token 2 -> row 5; 3 banned; 40 not in the subset");
    std::vector<float> da = d.run(picks);
    require(da[1] == dbase[1] + 50.f && da[4] == dbase[4] + 50.f, "both rows of a token listed twice get its bias");
    require(da[5] == NEG_INF && da[3] == NEG_INF, "-100 bans the token's row; the banned row stays banned at +100");
    require(da[0] == dbase[0] && da[2] == dbase[2], "rows of unbiased tokens are untouched");
    require(picks[0] == 4 && picks[1] == 4, "the draft's greedy pick follows the bias (row 4 = token 9, as the target)");
    sampler_clear_request_bias();
    require(same_bits(d.run(picks), dbase), "draft: cleared replays its no-bias baseline bit-identically");

    cudaFree(d_rows);
    std::puts("PASS: request logit bias - add, ban, no un-banning, graph replay A-then-B, reset, draft subset rows");
    return 0;
}
