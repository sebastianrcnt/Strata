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
python tools/steer_build.py tokens steer/profanity-ko.tsv steer/run
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
