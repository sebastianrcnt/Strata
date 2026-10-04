#include "strata/kernels/sampler.hpp"
#include "strata/kernels/verify_kernels.hpp"
#include <cuda_runtime.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void check(cudaError_t e) {
    if (e != cudaSuccess) { std::fprintf(stderr, "%s\n", cudaGetErrorString(e)); std::exit(1); }
}
static void require(bool ok, const char* what) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
template<class T> static T* upload(const std::vector<T>& v) {
    T* d = nullptr;
    check(cudaMalloc((void**) &d, v.size() * sizeof(T)));
    check(cudaMemcpy(d, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice));
    return d;
}
int main() {
    using namespace strata::kernels;
    const std::vector<int32_t> sub{5, 9, 1, 3};
    require(sampler_subset_bans(sub.data(), 4).empty(), "no bans leaves subset unchanged");
    const std::vector<float> logits{7, 90, 3, 80, 2, 90, 8, 80};
    float* d_l = upload(logits);
    int32_t* d_o = upload(std::vector<int32_t>(2, -1));
    float* d_p = upload(std::vector<float>(2, 0));
    SamplerParams greedy;
    greedy.greedy = true;
    apply_bans(d_l, 2, 4, nullptr);
    sample_tokens(d_l, 2, 4, nullptr, 0, greedy, d_o, nullptr);
    int32_t got[2];
    check(cudaMemcpy(got, d_o, sizeof(got), cudaMemcpyDeviceToHost));
    require(got[0] == 1 && got[1] == 1, "no-ban GPU path is a no-op");

    const int bans[]{9, 3, 9, -1, 99};
    sampler_set_bans(bans, 5);
    const auto rows = sampler_subset_bans(sub.data(), 4);
    require(rows == std::vector<int32_t>({1, 3}), "map token IDs to subset rows, deduplicate and omit absent IDs");
    require(sampler_subset_bans(std::vector<int32_t>{5, 1}.data(), 2).empty(), "absent bans leave allowed subset unchanged");
    int32_t* d_rows = upload(rows);

    // Greedy draft selection and confidence use the same masked logits, also when captured and replayed.
    cudaStream_t stream;
    check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    check(cudaStreamBeginCapture(stream, cudaStreamCaptureModeGlobal));
    apply_bans_rows(d_l, 2, 4, d_rows, (int) rows.size(), stream);
    sample_tokens(d_l, 2, 4, nullptr, 0, greedy, d_o, stream);
    row_top_prob(d_l, 2, 4, d_o, d_p, stream);
    cudaGraph_t graph;
    cudaGraphExec_t exec;
    check(cudaStreamEndCapture(stream, &graph));
    check(cudaGraphInstantiate(&exec, graph, nullptr, nullptr, 0));
    for (int replay = 0; replay < 3; ++replay) {
        check(cudaMemcpyAsync(d_l, logits.data(), logits.size() * sizeof(float), cudaMemcpyHostToDevice, stream));
        check(cudaGraphLaunch(exec, stream));
        check(cudaStreamSynchronize(stream));
        check(cudaMemcpy(got, d_o, sizeof(got), cudaMemcpyDeviceToHost));
        float probs[2];
        check(cudaMemcpy(probs, d_p, sizeof(probs), cudaMemcpyDeviceToHost));
        require(got[0] == 0 && got[1] == 2, "captured draft picks only allowed mapped rows");
        require(std::fabs(probs[0] - 1.f / (1.f + std::exp(-4.f))) < 1e-5f &&
                std::fabs(probs[1] - 1.f / (1.f + std::exp(-6.f))) < 1e-5f,
                "draft confidence renormalizes over allowed rows");
    }

    // Full-vocabulary masking uses the same token list; invalid ids are ignored by the kernel.
    std::vector<float> full(32, -20.f);
    for (int t = 0; t < 2; ++t) { full[t * 16 + 9] = 90; full[t * 16 + 3] = 80; }
    full[5] = 7; full[16 + 1] = 8;
    float* d_full = upload(full);
    apply_bans(d_full, 2, 16, nullptr);
    sample_tokens(d_full, 2, 16, nullptr, 0, greedy, d_o, nullptr);
    check(cudaMemcpy(got, d_o, sizeof(got), cudaMemcpyDeviceToHost));
    require(got[0] == 5 && got[1] == 1, "full and reduced heads agree on allowed token IDs");

    // Coupled sampling must also read the masked subset before selecting and writing its token to the ring.
    int32_t* d_sub = upload(sub);
    std::vector<int32_t> inv(16, -1);
    for (int i = 0; i < 4; ++i) inv[sub[i]] = i;
    int32_t* d_inv = upload(inv);
    int32_t* d_ring = upload(std::vector<int32_t>(33, -1));
    int32_t* d_step = upload(std::vector<int32_t>{0, 0, 0, 0});
    SamplerParams sampled;
    sampled.top_k = 1;
    sampled.top_p = 1;
    sampled.temperature = 1;
    sampled.seed = 42;
    SamplerParams* d_params = upload(std::vector<SamplerParams>{sampled});
    void* scratch = nullptr;
    check(cudaMalloc(&scratch, coupled_draft_scratch_bytes(4)));
    check(cudaMemcpy(d_l, logits.data(), logits.size() * sizeof(float), cudaMemcpyHostToDevice));
    apply_bans_rows(d_l, 1, 4, d_rows, (int) rows.size(), stream);
    coupled_draft_sample(d_l, 4, d_sub, d_inv, 16, d_params, d_ring, 32, 0, d_step, scratch, d_o, d_p, stream);
    check(cudaStreamSynchronize(stream));
    check(cudaMemcpy(got, d_o, sizeof(int32_t), cudaMemcpyDeviceToHost));
    require(got[0] == 5, "coupled draft excludes banned original IDs");
    int32_t ring_token;
    float coupled_prob;
    check(cudaMemcpy(&ring_token, d_ring + 32, sizeof(int32_t), cudaMemcpyDeviceToHost));
    check(cudaMemcpy(&coupled_prob, d_p, sizeof(float), cudaMemcpyDeviceToHost));
    require(ring_token == 5 && std::isfinite(coupled_prob) && coupled_prob > 0, "coupled ring and confidence stay valid");

    check(cudaGraphExecDestroy(exec)); check(cudaGraphDestroy(graph)); check(cudaStreamDestroy(stream));
    cudaFree(d_l); cudaFree(d_o); cudaFree(d_p); cudaFree(d_rows); cudaFree(d_full);
    cudaFree(d_sub); cudaFree(d_inv); cudaFree(d_ring); cudaFree(d_step); cudaFree(d_params); cudaFree(scratch);
    std::puts("PASS: no bans, full/subset IDs, greedy/confidence, CUDA graph replay, coupled sampling");
}
