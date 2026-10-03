# Temporary expert memory for GPU vision

Experimental, opt-in, single NVIDIA GPU only. Not yet validated with the full model or an actual mmproj encoder.
Do not enable production until the GPU integration test and image/text correctness tests pass on the chosen driver.

The normal resident encoder remains the default. Add expert_swap: true and headroom_mib: 2048 to the existing
vision section alongside gpu: true, exe, mmproj and model. The server sets STRATA_EXPERT_VMM=1 in the engine
environment. A rebuilt engine is required; old engines refuse the mode. HIP, multi-GPU and remote tiers are rejected.

The startup arena uses 64 MiB CUDA VMM chunks (rounded to device granularity), potentially one extra chunk over
cudaMalloc. On an image cache miss, the request FIFO waits for inference to finish. The engine commits pending
adaptive exchanges, synchronizes CUDA, snapshots the tail needed to reach headroom_mib free VRAM, then unmaps and
releases those chunks. Virtual addresses, slots, residency, KV, dense weights, MTP and conversations stay unchanged.
The encoder starts once for the batch and is reaped before the original addresses are remapped and bytes restored.
The snapshot preserves the actual adaptive expert bytes; the RAM pool may not contain every currently resident expert.

Headroom includes weights, work buffers and context overhead. 2048 MiB is a starting configuration, not a measured
guarantee. Measure peak usage at the configured image token limit and leave margin. Host RAM temporarily grows by
the freed amount, rounded to chunks. Setup's planner does not yet account for this snapshot. Separately measure
encoder startup, encoding and expert transfer latency.

Normalized cached images skip the handoff and encoder startup. Batches accept at most 64 images. Encoding errors,
startup errors and cancellation unwind through restoration. Failed restoration blocks generation until reload.
An encoder that cannot be reaped leaves memory lent and generation blocked, rather than overlapping GPU work.
Graph addresses remain stable, but replay after remapping still needs validation on the deployed driver.

Validation commands:

    python3 -m unittest serve.test_vision_swap serve.test_server.WebApp serve.test_server.ThinkingBudget
    ctest --test-dir build -R expert_cache_vision_test --output-on-failure

Build the GPU test with STRATA_ENABLE_CUDA=ON and STRATA_BUILD_TESTS=ON. It uses a synthetic 96 MiB cache, checks
physical free VRAM, byte-exact restoration, three handoffs and reuse of an instantiated CUDA graph. It skips when
less than 512 MiB is free. Run while the GPU is idle, without competing with a running inference server. Full-model
validation should cover cached/uncached and multiple images, invalid images, client disconnects, alternating text
and image requests, MTP, adaptive caching, conversation reuse, and output comparison before/after handoff.

No production service is restarted or production configuration changed by this feature branch.
