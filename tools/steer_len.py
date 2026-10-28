#!/usr/bin/env python3
"""Answer length and profanity count through a steering preset, on chat, coding and explanation prompts.

    python tools/steer_len.py TAG [--model orca-yok]
"""
import argparse
import json
import re
import urllib.request

PROMPTS = ["안녕", "요즘 뭐하고 지내?", "나 오늘 너무 힘들었어", "파이썬에서 리스트를 정렬하는 방법을 짧게 알려줘.",
           "점심 뭐 먹을까?", "이 코드 왜 안 돼? print(hello)", "블랙홀이 뭔지 설명해줘."]
SWEAR = re.compile("씨발|시발|존나|좆|새끼|개같|개빡|개좆|지랄|병신|처먹|뒤질|염병|개판|개소리")


def ask(url, model, prompt):
    body = {"model": model, "messages": [{"role": "user", "content": prompt}], "max_tokens": 800, "temperature": 0,
            "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url + "/v1/chat/completions", json.dumps(body).encode(),
                                 {"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=600) as r:
        d = json.load(r)
    return d["usage"]["completion_tokens"], d["choices"][0]["message"]["content"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--model", default="orca-yok")
    ap.add_argument("--url", default="http://127.0.0.1:8081")
    a = ap.parse_args()
    total = swears = 0
    for p in PROMPTS:
        n, text = ask(a.url, a.model, p)
        k = len(SWEAR.findall(text))
        total, swears = total + n, swears + k
        print(f"{a.tag} {p[:16]:18s} tok={n:4d} 욕={k:2d} | {' '.join(text.split())[:90]}")
    print(f"{a.tag} TOTAL tok={total} 욕={swears}")


if __name__ == "__main__":
    main()
