#!/usr/bin/env python3
"""A steering vector from a contrast set, for `--control-vector-scaled ... --cvec-mode add`.

The method is the contrastive one `gguf-neuron-explorer` and llama.cpp's cvector-generator use: run both sides of
a set of paired texts through the model, take each layer's mean residual on each side, and keep the difference.
Strata already applies such a vector after every steered layer's FFN write, on every hyper-connection stream
(`include/strata/kernels/cvec.hpp`), so all that is missing is the vector:

    python tools/steer_build.py tokens PAIRS.tsv OUTDIR              # OUTDIR/pos.txt, neg.txt (engine token ids)
    STRATA_PREFILL_SUMS=OUTDIR/pos.sums strata --tokens-file OUTDIR/pos.txt --max-new 1 ...   (and neg)
    python tools/steer_build.py build OUTDIR/pos.sums OUTDIR/neg.sums OUT.gguf

PAIRS.tsv is one pair per line, `negative<TAB>positive`: the same content without and with the behaviour. Each side
is fed as one stream of assistant turns through the batched prompt path, which with STRATA_PREFILL_SUMS sums every
layer's output residual over the tokens and the hc streams (`src/prefill/prefill.cpp`). `direction.<l>` is the mean
of side + minus the mean of side - at layer l's output; the engine adds it there times the file's scale.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
N_LAYERS, N_EMBD, HC = 48, 2560, 4


def tokenizer(path: Path):
    sys.path.insert(0, str(ROOT / "tools"))
    import strata_tokenizer as ST
    vocab = json.loads((path / "vocab.json").read_text(encoding="utf-8"))
    tokens = [None] * len(vocab)
    for t, i in vocab.items():
        tokens[i] = t
    merges = (path / "merges.txt").read_text(encoding="utf-8").split("\n")
    return ST.Tokenizer(tokens, merges, json.loads((path / "token_type.json").read_text()))


def cmd_tokens(a):
    tok = tokenizer(Path(a.tokenizer))
    pairs = [l.split("\t") for l in Path(a.pairs).read_text(encoding="utf-8").splitlines() if "\t" in l]
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    for side, name in ((0, "neg"), (1, "pos")):
        text = "".join(f"<|im_start|>assistant\n{p[side].strip()}<|im_end|>\n" for p in pairs)
        ids = tok.encode(text, parse_special=True)
        (out / f"{name}.txt").write_text(",".join(map(str, ids)))
        print(f"{name}: {len(pairs)} turns, {len(ids)} tokens")


def layer_means(path: Path) -> tuple[np.ndarray, int]:
    """A STRATA_PREFILL_SUMS file: int64 tokens, then (n_layers, n_embd) float64 sums over tokens and streams."""
    raw = path.read_bytes()
    tokens = int(np.frombuffer(raw[:8], np.int64)[0])
    sums = np.frombuffer(raw[8:], np.float64).reshape(N_LAYERS, N_EMBD)
    return sums / (tokens * HC), tokens


def cmd_build(a):
    pos, tp = layer_means(Path(a.pos))
    neg, tn = layer_means(Path(a.neg))
    diff = pos - neg
    print(f"positions: + {tp}, - {tn}")
    print("layer  |diff|   |mean|   ratio")
    for l in range(1, N_LAYERS):
        d, m = np.linalg.norm(diff[l]), np.linalg.norm((pos[l] + neg[l]) / 2)
        print(f"{l:5d} {d:8.3f} {m:8.3f} {d / m:7.3f}")
    sys.path.insert(0, str(ROOT / "third_party/llama.cpp/gguf-py"))
    import gguf   # llama.cpp's gguf-py: that checkout's, or PYTHONPATH / pip install gguf
    w =gguf.GGUFWriter(a.out, "controlvector")
    w.add_string("controlvector.model_hint", "qwen4exp")
    w.add_uint32("controlvector.layer_count", N_LAYERS - 1)
    for l in range(1, N_LAYERS):
        w.add_tensor(f"direction.{l}", diff[l].astype(np.float32))
    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"wrote {a.out}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    t = sub.add_parser("tokens")
    t.add_argument("pairs")
    t.add_argument("outdir")
    t.add_argument("--tokenizer", default=str(ROOT / "packs/sc117-gsq-rco-abliterated-iq3xxs/tokenizer"))
    b = sub.add_parser("build")
    b.add_argument("pos")
    b.add_argument("neg")
    b.add_argument("out")
    a = ap.parse_args()
    {"tokens": cmd_tokens, "build": cmd_build}[a.cmd](a)


if __name__ == "__main__":
    main()
