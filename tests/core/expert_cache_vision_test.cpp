// GPU-only opt-in integration test. Never loads a model; skip when the running service leaves little VRAM.
#include "strata/core/expert_cache.hpp"
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main() {
    size_t free_b = 0, total = 0;
    if (cudaMemGetInfo(&free_b, &total) != cudaSuccess || free_b < (512ULL << 20)) return 77;
#if defined(_WIN32)
    _putenv_s("STRATA_EXPERT_VMM", "1");
#else
    setenv("STRATA_EXPERT_VMM", "1", 1);
#endif
    strata::core::ExpertCache cache;
    std::string err;
    // Three 32 MiB slots span two 64 MiB VMM chunks. Fill the last (released) slot.
    if (!cache.open(3, 1, 3, 32ULL << 20, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    cache.admit(0, 0);
    cache.admit(0, 1);
    const int slot = cache.admit(0, 2);
    std::vector<uint8_t> bytes(32ULL << 20, 0xA7);
    if (slot != 2) return 1;
    // Use slot 2 explicitly so the captured address lies in the tail chunk.
    if (!cache.fill_slot_blocking(2, bytes.data(), err)) return 1;
    uint8_t* address = cache.device_slot(2);
    uint8_t* readback = nullptr;
    if (cudaMallocHost((void**) &readback, 1024) != cudaSuccess) return 1;
    cudaGraph_t graph = nullptr;
    cudaGraphExec_t exec = nullptr;
    cudaGraphNode_t node = nullptr;
    cudaStream_t replay = nullptr;
    if (cudaStreamCreateWithFlags(&replay, cudaStreamNonBlocking) != cudaSuccess) return 1;
    if (cudaGraphCreate(&graph, 0) != cudaSuccess ||
        cudaGraphAddMemcpyNode1D(&node, graph, nullptr, 0, readback, address, 1024,
                                cudaMemcpyDeviceToHost) != cudaSuccess ||
        cudaGraphInstantiate(&exec, graph, nullptr, nullptr, 0) != cudaSuccess) return 1;
    for (int i = 0; i < 3; ++i) {
        cudaMemGetInfo(&free_b, &total);
        if (!cache.lend_for_vision(free_b + 1, err) || !cache.lent()) {
            std::fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        if (cache.device_slot(2) != nullptr) return 1; // no direct cache copies may touch lent pages
        size_t after = 0;
        cudaMemGetInfo(&after, &total);
        if (after < free_b + (32ULL << 20)) return 1; // physical VRAM, not just residency flags
        if (cache.lend_for_vision(free_b, err)) return 1; // nested handoff refused
        if (!cache.reclaim_after_vision(err) || cache.lent() || cache.device_slot(2) != address ||
            cache.slot_of(0, 2) != slot) {
            std::fprintf(stderr, "%s\n", err.c_str()); return 1;
        }
        // Replay immediately on a nonblocking stream; no default-stream verification may mask restore ordering.
        if (cudaGraphLaunch(exec, replay) != cudaSuccess || cudaStreamSynchronize(replay) != cudaSuccess ||
            std::memcmp(readback, bytes.data(), 1024) != 0 || !cache.verify_slot(2, bytes.data(), err)) return 1;
    }
    cudaStreamDestroy(replay);
    cudaGraphExecDestroy(exec);
    cudaGraphDestroy(graph);
    cudaFreeHost(readback);
    std::puts("VMM physical release, byte-exact restore, stable addresses and graph reuse passed");
    return 0;
}
