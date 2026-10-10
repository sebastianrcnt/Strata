// include/strata/kernels/sampler.hpp - the sampler chain, host-callable (P2.S2).
//
//     penalties -> top_k -> top_p -> min_p -> temperature -> pick
//
// llama.cpp's default chain (issue #53): ONE penalties stage, first, and temperature AFTER the truncation filters;
// top_p cuts before min_p.  `src/kernels/cuda/sampler.cu` has the details and `sampler_parity` pins them.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace strata::kernels {

struct SamplerParams {
    int top_k = 20;              // sampled path: 1..64 as given; 0 (off) or more than 64 keep the widest list, 64
    float top_p = 0.95f;         // 1.0 disables the filter
    float min_p = 0.0f;          // 0 disables; keeps tokens with p >= min_p * p_max (a prefix of the top_k list)
    float temperature = 1.0f;    // <= 0 means greedy
    int min_keep = 1;            // top_p keeps at least this many
    int penalty_last_n = 0;      // 0 disables; the window over `history` to count occurrences in
    float penalty_repeat = 1.0f;
    float penalty_freq = 0.0f;
    float penalty_present = 0.0f;
    uint64_t seed = 0;           // drives Philox, which is counter-based on (seed, token index)
    uint64_t counter = 0;        // absolute draw index of row 0; advance across decode calls
    bool greedy = false;
};

// --ban-tokens (local): token ids whose logits are set to -inf before every verify-window pick.  Set once at start,
// before any graph capture; apply_bans is a no-op without a list (and without a request bias, below).
void sampler_set_bans(const int* ids, int n);
// The bans, then the request bias of the target set (token ids): the verify head, and a draft head over the whole
// vocabulary.
void apply_bans(float* logits, int n_tokens, int n_vocab, void* stream);
// Map the same bans to a draft head's vocabulary. `sub_to_id` is a HOST array; the returned ids index its rows.
// Called once before graph capture. The row list passed to apply_bans_rows is a DEVICE array, owned by the caller.
std::vector<int32_t> sampler_subset_bans(const int32_t* sub_to_id, int n_sub);
// Which request bias set (below) a ban launch also applies: none, the target's token ids, or the draft head's rows.
enum class BiasRows { None, Target, Draft };
void apply_bans_rows(float* logits, int n_tokens, int n_vocab, const int32_t* rows, int n_rows, void* stream,
                     BiasRows bias = BiasRows::None);

// Per-request logit bias (OpenAI `logit_bias`), on top of the --ban-tokens base.
//
// CUDA-GRAPH SAFE BY CONSTRUCTION.  The verify window and the MTP draft step are captured graphs, which bake in every
// kernel argument - so a request's count or pointer can never be passed in.  Instead each set (target, draft) is ONE
// device buffer allocated once, before any capture, at a fixed address: [count, pad, (id, value) x capacity].  The
// ban kernel reads the count from that buffer on every replay and adds the entries after the bans.  A request writes
// its entries (and count) before its first window and the count goes back to 0 after it; with count 0 the kernel
// writes nothing, so a request without a bias gets exactly today's logits (no new launch either: the ban launch
// carries it).
//
// Semantics, as OpenAI's: value in [-100, 100] is ADDED to the logit before the sampler chain (penalties, top_k,
// top_p, min_p, temperature); -100 is a hard ban (-inf); +100 all but forces the token.  A banned token (--ban-tokens)
// stays banned: its entries are dropped on the host, so a request can never un-ban one (and no entry ever shares an
// address with a ban write).  Ids repeated in one request: the last value wins.  0 is a no-op and dropped.
constexpr int kMaxRequestBias = 1024;       // entries per request (the server allows OpenAI's 300)
constexpr float kRequestBiasBan = -100.0f;  // this value or below: -inf
// Allocate both fixed buffers (the draft's only once a subset is known).  Once, before any graph capture.
void sampler_init_request_bias();
// The draft head's row -> token id map (a HOST array), so the draft sees the same bias on its own rows.  Once, before
// the draft graphs are captured.
void sampler_set_draft_subset(const int32_t* sub_to_id, int n_sub);
// This request's entries (HOST arrays; ids are token ids, already checked against the vocabulary).  n = 0 clears.
// Synchronous: the buffers hold the entries when it returns, so the next window launched on any stream sees them;
// call it between requests only (no window or draft in flight).  false (and `err`) on bad input or a CUDA error.
bool sampler_set_request_bias(const int32_t* ids, const float* values, int n, std::string& err);
void sampler_clear_request_bias();
// Entries in effect now: the target set (after dropping banned and 0 entries) and the draft set (rows).
int sampler_request_bias_entries();
int sampler_request_bias_draft_entries();

// logits (n_tokens, n_vocab) -> one sampled token id per row in `out`.
//
// **`logits` AND `out` ARE DEVICE POINTERS.**  This is a CUDA kernel launch, not a host function, and nothing
// in the parameter names or the types says so - `const float* logits` reads exactly like a host buffer.
// Measured: passing host memory for `logits` faults inside the kernel with an ILLEGAL MEMORY ACCESS that is
// reported by whatever synchronising call happens NEXT, which will be somewhere else entirely and will name a
// buffer that has nothing to do with it.  A sticky async fault does not know where it came from.
//
// `history` is (n_tokens, history_len) int32 - ONE ROW PER TOKEN ROW, at a stride of `history_len` - the most
// recent tokens that row's pick follows, with any unused slots set to -1; only the last `p.penalty_last_n` of
// each row are counted, and ids outside [0, n_vocab) are ignored.  Pass nullptr and 0 when no penalties apply.
// A verify window's rows need DIFFERENT histories: row t follows the window's drafts 1..t (`penalty_rows`).
void sample_tokens(const float* logits, int n_tokens, int n_vocab, const int* history, int history_len,
                   const SamplerParams& p, int* out, void* stream);

// ---- COUPLED DRAFT SAMPLING (include/strata/core/coupled_draft.hpp, STRATA_SPEC_COUPLED=1).  Device pointers
// throughout; every per-request / per-round value comes from device memory, so the calls can be captured.
//
// The scratch `coupled_draft_sample` needs for `nv` logits (0: too wide for the split merge, coupled cannot run).
size_t coupled_draft_scratch_bytes(int nv);
// A round's inputs: `*params` <- `*mapped_params`, ring[cap - h, cap) <- mapped_hist[cap - h, cap) with h =
// coupled_hist_len(penalty_last_n, cap).  `mapped_*` are mapped host memory.
void coupled_draft_stage(const SamplerParams* mapped_params, const int32_t* mapped_hist, SamplerParams* params,
                         int32_t* ring, int cap, void* stream);
// Draft j of the chain from ONE row of `nv` logits (modified in place: the penalties): the target's chain with
// `*params`, the penalty window ring[cap + j - h, cap + j), Philox counter coupled_draft_counter(step_rec[0]).
// `sub_to_id` / `id_to_sub` map the draft head's subset to token ids and back (null: the whole vocabulary,
// `id_vocab` ids).  Writes the token id to *out_id, its probability under the final distribution to *out_prob, and
// the id to ring[cap + j].
void coupled_draft_sample(float* logits, int nv, const int32_t* sub_to_id, const int32_t* id_to_sub, int id_vocab,
                          const SamplerParams* params, int32_t* ring, int cap, int j, const int32_t* step_rec,
                          void* scratch, int32_t* out_id, float* out_prob, void* stream);

// The penalty-history rows of a verify window, on the host: row t of `out` (T rows of `h` slots) is the last `h`
// tokens of `tail[0..n_tail)` followed by `window[0..t]`, most recent LAST, -1 in the unused front slots.
// `tail` is what the state consumed before the window, `window[0]` the fed-back token and `window[1..]` the
// drafts: row t is exactly the history plain decode counts when it picks the token after `window[t]`, so drafting
// cannot change which tokens are penalised.  Row 0 alone is the single row the engine staged before 0.1.19.
inline void penalty_rows(const int32_t* tail, int64_t n_tail, const int32_t* window, int T, int h, int32_t* out) {
    for (int t = 0; t < T; ++t) {
        int32_t* row = out + (size_t) t * (size_t) h;
        const int64_t avail = n_tail + t + 1;                      // tail + window[0..t]
        const int take = (int) std::min<int64_t>(h, avail);
        std::fill(row, row + (h - take), -1);
        for (int j = 0; j < take; ++j) {
            const int64_t i = avail - take + j;                     // index into (tail, window)
            row[h - take + j] = i < n_tail ? tail[i] : window[i - n_tail];
        }
    }
}

}  // namespace strata::kernels
