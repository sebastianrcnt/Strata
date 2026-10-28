#!/usr/bin/env python3
"""Same prompts with the loaded control vector off and on, through the server's per-request switch.

    python tools/steer_ab.py [--url http://127.0.0.1:8081] [--max-tokens 120] > OUT.md
"""
import argparse
import json
import time
import urllib.request

PROMPTS = [
    "오늘 하루 어땠는지 두세 문장으로 말해줘.",
    "파이썬에서 리스트를 정렬하는 방법을 짧게 알려줘.",
    "회사 동료가 회의에 또 늦었어. 어떻게 말하면 좋을까?",
    "비 오는 날 어울리는 음식 추천해줘.",
    "How was your weekend? Answer in two sentences.",
]


def ask(url, prompt, on, max_tokens):
    body = {
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "temperature": 0,
        "seed": 1,
        "chat_template_kwargs": {"enable_thinking": False},
        "experimental_speed_projection": on,
    }
    req = urllib.request.Request(url + "/v1/chat/completions", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        return json.load(r)["choices"][0]["message"]["content"].strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://127.0.0.1:8081")
    ap.add_argument("--max-tokens", type=int, default=120)
    a = ap.parse_args()
    for p in PROMPTS:
        print(f"### {p}\n")
        for on in (False, True):
            t = time.time()
            out = ask(a.url, p, on, a.max_tokens)
            print(f"**{'ON ' if on else 'OFF'}** ({time.time() - t:.1f}s): {out}\n", flush=True)


if __name__ == "__main__":
    main()
