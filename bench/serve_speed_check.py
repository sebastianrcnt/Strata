#!/usr/bin/env python3
"""A short, polite speed + output check against a running Strata server (the logit_bias deploy gate).

Runs, only while the server is idle (/slots not processing, nothing else in flight):
  * one warm-up run of the fixed prompt (not counted: a prompt the conversation cache does not hold yet decodes
    slower on its first run), then N runs (--runs) of it, temperature 0, thinking off, max_tokens 512: decode tok/s
    (timings.predicted_per_second, the median is compared) and the exact output text;
  * P prefill-heavy runs (--prefill-runs): about 4k tokens of input that start with a fresh nonce (so the
    conversation cache cannot hold any of it), max_tokens 16: prefill tok/s (timings.prompt_per_second, median).
Optional functional checks for logit_bias (--bias-checks, only on a build that supports it).

NOTE: with the adaptive expert tier the engine is NOT deterministic run to run even at temperature 0 (docs/DETAILS.md;
measured 2026-10-11 on spill-71fedeef: three runs of the same prompt diverged after 539 and 1822 characters).  So
the outputs are compared and reported (identical or not, common prefix), but only --strict-outputs gates on them.

Usage: serve_speed_check.py --url http://127.0.0.1:8081 --out result.json [--compare baseline.json] [--bias-checks]
With --compare it prints the speed change versus the baseline and how the outputs compare; it exits 1 when the decode
or prefill median is more than --tolerance (default 1 %) below the baseline's (or, with --strict-outputs, when an
output differs), or a --bias-checks check fails.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
import uuid

PROMPT = ("Explain how TCP congestion control works: slow start, congestion avoidance, fast retransmit and fast "
          "recovery. Use a few short sections with plain prose.")
FILLER_TOPICS = ["rivers", "bridges", "printing presses", "lighthouses", "clocks", "railways", "telescopes",
                 "windmills", "canals", "observatories", "aqueducts", "harbours", "libraries", "looms", "kilns"]


def http(url, body=None, timeout=900):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"} if data else {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_idle(base, max_wait=3600):
    """Until the server runs nothing and queues nothing (checked twice, 3 s apart); gives up after max_wait s."""
    t0, calm = time.time(), 0
    while time.time() - t0 < max_wait:
        _, slots = http(base + "/slots", timeout=10)
        _, st = http(base + "/v1/status", timeout=10)
        busy = any(s.get("is_processing") for s in slots) if isinstance(slots, list) else True
        others = ((st or {}).get("activity") or {}).get("in_flight", 1) - 1     # minus this status request
        calm = calm + 1 if not busy and others <= 0 else 0
        if calm >= 2:
            return
        time.sleep(3)
    sys.exit("server stayed busy; not benchmarking against real users")


def chat(base, content, max_tokens, **extra):
    body = {"model": "x", "messages": [{"role": "user", "content": content}], "temperature": 0,
            "max_tokens": max_tokens, "reasoning_effort": "none", "stream": False, **extra}
    t0 = time.time()
    code, r = http(base + "/v1/chat/completions", body)
    wall = time.time() - t0
    if code != 200:
        return {"http": code, "error": r}
    t = r.get("timings") or {}
    return {"http": 200, "text": r["choices"][0]["message"].get("content"), "finish": r["choices"][0]["finish_reason"],
            "usage": r.get("usage"), "timings": t, "wall_s": round(wall, 2),
            "decode_tok_s": t.get("predicted_per_second"), "prefill_tok_s": t.get("prompt_per_second")}


def long_prompt(nonce):
    lines = [f"Reference id {nonce}. Read the notes below, then answer the question at the end in one sentence."]
    i = 0
    while len(lines) < 112:                            # ~4k tokens with this tokenizer
        topic = FILLER_TOPICS[i % len(FILLER_TOPICS)]
        lines.append(f"Note {i + 1}: the {topic} of district {i * 7 % 101} were surveyed in year {1700 + i * 3}, "
                     f"and the record lists {i * 13 % 97} items in good repair.")
        i += 1
    lines.append("Question: in which year were the windmills of the first district with windmills surveyed?")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8081")
    ap.add_argument("--out", required=True)
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--prefill-runs", type=int, default=2)
    ap.add_argument("--no-warmup", action="store_true")
    ap.add_argument("--strict-outputs", action="store_true")
    ap.add_argument("--compare")
    ap.add_argument("--tolerance", type=float, default=0.01)
    ap.add_argument("--bias-checks", action="store_true")
    ap.add_argument("--repo", default="/home/coolguy/ml/strata", help="for tools/strata_tokenizer.py")
    ap.add_argument("--tokenizer", default="/home/coolguy/ml/strata/packs/sc117-gsq-rco-abliterated-iq3xxs/tokenizer")
    a = ap.parse_args()
    base = a.url.rstrip("/")
    res = {"when": time.strftime("%Y-%m-%dT%H:%M:%S%z"), "url": base, "prompt": PROMPT, "runs": [], "prefill": []}
    _, res["health"] = http(base + "/health", timeout=10)
    _, st = http(base + "/v1/status", timeout=10)
    res["engine"], res["uptime_s"] = (st or {}).get("engine"), (st or {}).get("uptime_s")
    if not a.no_warmup:
        wait_idle(base)
        r = chat(base, PROMPT, 512)
        print(f"warm-up: decode {r.get('decode_tok_s')} tok/s (not counted)", flush=True)
        res["warmup"] = r
    for i in range(a.runs):
        wait_idle(base)
        r = chat(base, PROMPT, 512)
        print(f"run {i + 1}: decode {r.get('decode_tok_s')} tok/s, prefill {r.get('prefill_tok_s')} tok/s, "
              f"{(r.get('usage') or {}).get('completion_tokens')} tokens, cache_n {r['timings'].get('cache_n')}",
              flush=True)
        res["runs"].append(r)
    for i in range(a.prefill_runs):
        wait_idle(base)
        r = chat(base, long_prompt(uuid.uuid4().hex), 16)
        print(f"prefill-heavy {i + 1}: {r.get('prefill_tok_s')} tok/s over {r['timings'].get('prompt_n')} tokens "
              f"(cache_n {r['timings'].get('cache_n')})", flush=True)
        res["prefill"].append(r)
    texts = [x.get("text") for x in res["runs"]]
    dec = [x["decode_tok_s"] for x in res["runs"]]
    pre = [x["prefill_tok_s"] for x in res["prefill"]]
    res["summary"] = {"decode_tok_s_median": statistics.median(dec), "decode_tok_s_all": dec,
                      "decode_spread": round((max(dec) - min(dec)) / statistics.median(dec), 4),
                      "prefill_heavy_tok_s": statistics.median(pre), "prefill_heavy_all": pre,
                      "prefill_heavy_prompt_n": res["prefill"][0]["timings"].get("prompt_n"),
                      "outputs_identical_across_runs": len(set(texts)) == 1}

    if a.bias_checks:
        res["bias"] = bias_checks(base, texts[0], load_tokenizer(a.repo, a.tokenizer))

    ok = True
    if a.compare:
        b = json.load(open(a.compare))
        bs, s = b["summary"], res["summary"]
        dd = s["decode_tok_s_median"] / bs["decode_tok_s_median"] - 1
        dp = s["prefill_heavy_tok_s"] / bs["prefill_heavy_tok_s"] - 1
        import os
        btexts = [y.get("text") or "" for y in b["runs"]]
        same = [t in btexts for t in texts]
        prefix = [max(len(os.path.commonprefix([t or "", y])) for y in btexts) for t in texts]
        res["compare"] = {"baseline": a.compare, "decode_change": round(dd, 4), "prefill_change": round(dp, 4),
                          "baseline_decode_spread": bs.get("decode_spread"),
                          "outputs_equal_to_a_baseline_output": same, "longest_common_prefix_chars": prefix,
                          "outputs_bit_identical": all(same)}
        print(json.dumps(res["compare"]))
        ok = dd >= -a.tolerance and dp >= -a.tolerance and (all(same) or not a.strict_outputs)
    if a.bias_checks:
        ok = ok and all(v.get("pass") for v in res["bias"].values())
    json.dump(res, open(a.out, "w"), indent=1, ensure_ascii=False)
    print(json.dumps(res["summary"]))
    sys.exit(0 if ok else 1)


def load_tokenizer(repo, tdir):
    """The server's own tokenizer (serve/server.py builds it the same way from the pack's tokenizer dir)."""
    from pathlib import Path
    sys.path.insert(0, str(Path(repo) / "tools"))
    import strata_tokenizer as ST
    tpath = Path(tdir)
    vocab = json.loads((tpath / "vocab.json").read_text(encoding="utf-8"))
    tokens = [None] * len(vocab)
    for t, i in vocab.items():
        tokens[i] = t
    return ST.Tokenizer(tokens, (tpath / "merges.txt").read_text(encoding="utf-8").split("\n"),
                        json.loads((tpath / "token_type.json").read_text()))


def bias_checks(base, baseline_text, tok):
    """-100 on a token the baseline answer used removes it; +bias forces a token; a bad logit_bias is a 400."""
    out = {}
    # the token: the baseline answer's most frequent token that is a whole word (" congestion" is the likely one)
    base_ids = tok.encode(baseline_text)
    words = [i for i in base_ids if tok.decode([i]).strip().isalpha() and len(tok.decode([i]).strip()) > 4]
    tid = max(set(words), key=words.count)
    wait_idle(base)
    r = chat(base, PROMPT, 512, logit_bias={str(tid): -100})
    used = tid in tok.encode(r.get("text") or "")
    out["ban"] = {"token": tid, "piece": tok.decode([tid]), "baseline_count": words.count(tid),
                  "biased_has_token": used, "text": r.get("text"), "pass": r.get("http") == 200 and not used}
    # +100 on one token: the answer is that token over and over (greedy: it wins every step it is not banned)
    ids = tok.encode(" banana")
    if ids:
        wait_idle(base)
        r = chat(base, "Say hello.", 8, logit_bias={str(ids[0]): 100})
        out["force"] = {"token": ids[0], "text": r.get("text"),
                        "pass": r.get("http") == 200 and (r.get("text") or "").count("banana") >= 4}
    for name, bad in (("too_big", {"1": 101}), ("not_int", {"abc": 1}), ("out_of_vocab", {"999999999": 1}),
                      ("too_many", {str(i): 1 for i in range(301)}), ("not_number", {"5": "x"})):
        code, r = http(base + "/v1/chat/completions",
                       {"model": "x", "messages": [{"role": "user", "content": "hi"}], "max_tokens": 1,
                        "logit_bias": bad})
        out["bad_" + name] = {"http": code, "pass": code == 400}
    return out


if __name__ == "__main__":
    main()
