# Steering vectors (feat/steering)

A contrastive steering vector for Strata, after the method of
[gguf-neuron-explorer](https://github.com/balaena01/gguf-neuron-explorer) (no code from it): run both sides of a
paired contrast set through the model, keep each layer's mean residual difference, and load it with the engine's
existing `--control-vector-scaled ... --cvec-mode add`, which a request switches with
`"experimental_speed_projection": true|false`.

The engine change is one env-gated hook: `STRATA_PREFILL_SUMS=FILE` makes the batched prompt path write every
layer's output residual summed over the tokens and the hc streams (`src/prefill/prefill.cpp`). Unset, nothing
changes. The per-token `--dump-layers` cannot be used for this: an IQ pack needs `--spec`, which needs the token
graph, which the layer dump turns off.

## Files

| | |
| --- | --- |
| `profanity-ko.tsv` | 48 pairs, polite Korean vs the same content with profanity |
| `profanity-ko.gguf` | the vector (`direction.1..47`, 2,560 f32 each), from 820 / 997 prompt tokens |
| `../tools/steer_build.py` | `tokens` (pairs -> engine token files), `build` (two sums files -> controlvector GGUF) |
| `../tools/steer_ab.py` | the same prompts off and on through the server |
| `engine.sh`, `serve_steer.sh` | this machine's docker paths: one engine run, or the service with a vector loaded (off by default) |

## Rebuild

```
python tools/steer_build.py tokens steer/profanity-ko.tsv steer/run --tokenizer <pack>/tokenizer
STRATA_PREFILL_SUMS=/work/run/pos.sums steer/engine.sh --prefill 512 --spec 4 --tokens-file /work/run/pos.txt --max-new 1   # and neg
python tools/steer_build.py build steer/run/pos.sums steer/run/neg.sums steer/profanity-ko.gguf
steer/serve_steer.sh 0.15 8 32
```

## Measured (RTX 3090, IQ3_XXS sc117-gsq-rco-abliterated, greedy, 5 prompts, `ab-*.md`)

| Scale, layers 8-32 | Result with the vector on |
| --- | --- |
| 0.15 | casual speech with profanity, coherent; code answers get terse (`lst.sort()` only) |
| 0.2 | more profanity, first broken words |
| 0.3 | broken words, answers cut short |

Off, answers are the stock model's; they vary between requests by themselves (the first of three identical greedy
requests differed), so off-vs-stock is not compared token for token here. Not measured: KL / perplexity cost.

## Model-written contrast set (`profanity-ko-gen.*`)

`tools/gen_pairs.py --style "욕을 섞은 거친 반말" --n 72` had the stock model (orca-local, vector off) write 72
questions, answer each plainly, rewrite the answer in the style with the content kept, and judge the rewrite: 56 pairs
kept (3,230 / 3,672 tokens). Same build, same 5 prompts:

| Vector | Scale 8-32 | Profanity words in the 5 answers | Note |
| --- | --- | ---: | --- |
| hand-written 48 pairs | 0.15 | 2 | rough, coherent; code answer terse |
| hand-written | 0.2 | 7 | first broken words |
| model-written 56 pairs | 0.15 | 0 | plain 반말 only |
| model-written | 0.25 | 0 | 반말, curt |
| model-written | 0.35 | 5 | rambling starts; the code answer stays useful |

The model-written vector is ~30% smaller (|diff| at layer 16: 0.119 vs 0.175) and points elsewhere in part (cosine
with the hand-written one 0.55-0.66 per layer). Its rewrites carry 65 profanity words in 56 longer answers against 95
in 48 short lines, so it learned "rough casual tone" more than profanity. The service keeps the hand-written vector.

The reference (gguf-neuron-explorer `gne direction --positions response`) differs again: persona and neutral system
prompts, replies generated independently, the conversation prefilled with the system prompt kept, and only the reply
tokens averaged, each conversation on a cleared context. Not tried here: the sums hook has no per-position mask, and
the streams here are concatenated, so later turns see earlier ones.

## Short + long mix (`profanity-ko-long.*`, `profanity-ko-mix.gguf`) - the served vector

The hand-written pairs are one-liners, so the vector also learned "curt chat": answers got 30-60% shorter and a
"briefly" request came back as one line. `gen_pairs.py --system "" --answer-tokens 350 --rewrite "문장마다 ... 욕을
하나 이상" --require ...` made 48 long, formatted answer pairs (16,200 / 18,869 tokens); `steer_build.py build ...
--also` averages the short and the long set's differences with equal weight (cosine short vs long 0.49-0.71, the mix
at layer 16 is 0.113 against the short set's 0.175). `tools/steer_len.py`, 7 prompts, greedy, max 800 tokens:

| Vector, layers 8-32 | Tokens, all 7 | Profanity words | Black hole / code question |
| --- | ---: | ---: | --- |
| stock | 2,131 | 0 | 800 / 323 |
| short set 0.12 | 1,412 | 3 | |
| short set 0.15 | 903 | 5 | 205 / 93 |
| mix 0.12 | 1,786 | 0 | |
| mix 0.18 | 1,256 | 3 | |
| **mix 0.22** | **1,480** | **7** | **705 / 37** |

Mix 0.22 swears more than the short set at 0.15 and keeps explanations long; a "briefly" request is still one line,
and the code question got shorter. The long rewrites insert some profanity mechanically (the model was told to put
one in every sentence). Served: `steer/serve_steer.sh 0.22 8 32 profanity-ko-mix.gguf`.

## Steering studio (feat/steering-studio)

Vectors made while the model serves, and a page to make, compare, keep and chat with them
(`tools/steer_studio.py`, Gradio). Nothing here restarts the engine.

- **Engine.** `GEN ... sums=FILE` reads that request's prompt with the stock model through the batched path, from
  token 0, and writes the residual sums as `STRATA_PREFILL_SUMS` does (`prefill_sums_arm`). `CVEC_LOAD <id> <scale>
  <first> <last> <file>` puts another vector (raw `n_layers x n_embd` f32) into the start vector's device buffers,
  so the CUDA graphs stay valid. A request then asks for it with `cvec=<id>` (2..255). The conversation caches key on
  that id instead of on/off. The disk tier keeps 0 and 1 only, because an id names nothing in another run.
  `--cvec-cover 1 47` lets a loaded vector steer every layer. Each covered layer costs an unfused residual write,
  which was not measurable here (70-90 tok/s either way).
- **Server.** `serve/steering.py`, config `"steering": {"dir": "/state/steer"}`. `/steer/measure|draft|save|update|
  delete|status` need the token in `<dir>/admin.token` (header `X-Steer-Token`). A request uses a saved vector with
  `"model": "steer-NAME"` or `"strata_steer": "NAME"` (`"draft"`: the last measured one). Each layer's row is set to
  the length of that layer's mean residual, so a scale is the share of the residual it pushes by. 0.025 means 2.5%,
  and the orca-yok vector at 0.22 is about 2.6% in these units. One pair measures in about 1 s.
- **Page.** `steer-studio.service` (systemd --user) listens on 127.0.0.1 and the tailnet address only, port 7870.
