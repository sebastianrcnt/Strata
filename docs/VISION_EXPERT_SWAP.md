# Temporary expert memory for GPU vision

Opt-in, single NVIDIA GPU only. Validated on an RTX 3090 with the Qwen3.8-Flash-Next ORCA model and the matching
BF16 mmproj. Other GPU/driver combinations must run the integration and image/text checks before deployment.

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
Restoration synchronizes CUDA before acknowledgment; graph replay on a nonblocking stream passed after remapping
on the validation machine. Validate this on a different deployed driver.

Validation commands:

    python3 -m unittest serve.test_vision_swap serve.test_server.WebApp serve.test_server.ThinkingBudget
    ctest --test-dir build -R expert_cache_vision_test --output-on-failure

Build the GPU test with STRATA_ENABLE_CUDA=ON and STRATA_BUILD_TESTS=ON. It uses a synthetic 96 MiB cache, checks
physical free VRAM, byte-exact restoration, three handoffs and reuse of an instantiated CUDA graph. It skips when
less than 512 MiB is free. Run while the GPU is idle, without competing with a running inference server. Full-model
validation should cover cached/uncached and multiple images, invalid images, client disconnects, alternating text
and image requests, MTP, adaptive caching, conversation reuse, and output comparison before/after handoff.

The code does not restart production or change deployment configuration by itself.

A timeout or broken control pipe blocks both generation and all future handoff commands until an explicit engine
reload. Late replies are never reused as acknowledgments for a new handoff. An explicit engine ERR response permits
the normal restoration attempt. A separately specified vision cuda_device must be a non-negative integer matching
an explicitly configured engine GPU; omitting it lets both children inherit the same CUDA device visibility.

Encoder startup and response reads are bounded and cancellation-aware: start_timeout_s defaults to 120 seconds,
encode_timeout_s to 180 seconds (each configurable up to 600). The HTTP disconnect watcher is active during image
preparation. Startup EOF/ERR/timeouts become handled HTTP service errors after encoder reaping and expert rollback.
Live-child tests cover an actual HTTP disconnect while the encoder is silent, for both API dialects. A failed or
uncertain handoff makes conversation persistence fail immediately; backend ERR replies are not ignored.
Successful handoffs log separate release/load/encode/unload/restore seconds for measurement.


## Measured validation (2026-10-04)

RTX 3090 24 GiB, Ryzen 5 5600X, 78.4 GiB usable RAM, driver 595.84, CUDA 13.2 engine, ORCA BF16-derived ISTA allocation GGUF.
Vision helper built from llama.cpp 3cf03257f219afbe7334045ff7c6a06ac68c627d, matching 907,543,008-byte BF16 mmproj,
GPU encoding with max_tokens 1024 and headroom_mib 2048. These figures cover this configuration, not all images.

- 121 Python tests passed, including real HTTP disconnection from a silent live helper for both API dialects,
  bounded startup/encoding failures, immediate persistence ERR handling, and unsafe reload blocking.
- Actual GPU test passed three release/restore cycles, physical VRAM freeing, exact restored expert bytes,
  stable pointers, and captured graph replay immediately on a nonblocking stream after restoration.
- Full-model API checks passed: red image OCR (ORCA 42), blue/green two-image order, Anthropic red classification,
  cached images, invalid-image HTTP 400 followed by successful text, actual HTTP disconnect followed by text,
  MTP/adaptive 391-token decode, and exact matching text before and after handoff.
- One cold image took 3.682 s total: release 1.1464 s, encoder load 1.4530 s, encode 0.0716 s,
  unload 0.1136 s, synchronized restore 0.3409 s. The remaining time includes prompt/decode/server work.
  The cached repeat took 0.495 s and performed no expert handoff or encoder startup.
- Two images used one handoff, encoding 0.1437 s, total 5.541 s including prompt/decode.
- A fresh 4096×3072 image after the adaptive/MTP run returned VISION 73 correctly in 5.675 s;
  the resulting prompt used 1154 tokens, exercising the configured 1024-image-token limit. Its cached repeat
  classified Yellow without a handoff, and text afterward matched the baseline.
- Sampled free VRAM increased from approximately 334–448 MiB to 2056–2062 MiB during lending;
  minimum available host RAM over the suite and large-image check was 17,680 MiB. Sampling interval was 50 ms.
  Experts stayed at 8980 slots / 17,391 MiB after lending; KV and parked conversations remained usable.

The normal CUDA VMM allocation rounds the expert arena upward; this validation had two fewer expert slots than
the previous cudaMalloc arena. Capacity is preserved across each handoff, not necessarily identical between
allocator modes. There is no permanent resident vision encoder. Setup planning still needs to account for the
temporary host snapshot. Cancellation cannot preempt a CUDA driver operation already in progress.
