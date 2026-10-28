#!/usr/bin/env python3
"""A contrast set written by the model itself, for tools/steer_build.py.

    python tools/gen_pairs.py --style "욕을 섞은 거친 반말" --n 60 steer/profanity-ko-gen.tsv

Both sides follow the model's own distribution: it writes the questions, answers them plainly, then rewrites each
answer in the style with the content kept, so the pair differs in the style and little else. A pair is dropped when
the rewrite is much shorter or longer than the answer, or when the model, asked as a judge, says the rewrite changed
the content or missed the style. Send it to the stock model (with any steering off), or the vector leaks into the
data. Only the answers are written out: the instructions that produced them are not part of what gets measured.
"""
from __future__ import annotations

import argparse
import json
import random
import re
import sys
import urllib.request

TOPICS = ["일상과 날씨", "음식과 요리", "프로그래밍과 코드", "회사와 인간관계", "건강과 운동", "여행", "돈과 소비",
          "공부와 시험", "게임과 취미", "기술 제품", "연애와 친구", "집안일과 생활 팁"]


class Model:
    def __init__(self, url: str, model: str):
        self.url, self.model = url.rstrip("/"), model

    def ask(self, prompt: str, max_tokens: int, temperature: float = 0.7, system: str | None = None) -> str:
        msgs = ([{"role": "system", "content": system}] if system else []) + [{"role": "user", "content": prompt}]
        body = {"model": self.model, "messages": msgs, "max_tokens": max_tokens, "temperature": temperature,
                "chat_template_kwargs": {"enable_thinking": False}, "experimental_speed_projection": False}
        req = urllib.request.Request(self.url + "/v1/chat/completions", json.dumps(body).encode(),
                                     {"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=600) as r:
            return json.load(r)["choices"][0]["message"]["content"].strip()


def one_line(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip()


def questions(m: Model, n: int) -> list[str]:
    out: list[str] = []
    per = max(3, -(-n // len(TOPICS)))
    for topic in TOPICS:
        text = m.ask(f"'{topic}'에 관해 사람들이 AI에게 물어볼 법한 짧은 질문 {per}개를 써 줘. "
                     "한 줄에 질문 하나씩, 번호나 다른 말 없이 질문만.", 400, temperature=1.0)
        for line in text.splitlines():
            q = re.sub(r"^\s*(\d+[.)]|[-*•])\s*", "", line).strip()
            if 5 <= len(q) <= 120 and q not in out:
                out.append(q)
    random.Random(0).shuffle(out)
    return out[:n]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out", help="the pairs, plain<TAB>styled per line")
    ap.add_argument("--style", required=True, help="the style in words, e.g. '욕을 섞은 거친 반말'")
    ap.add_argument("--n", type=int, default=60, help="questions to ask (some pairs get dropped)")
    ap.add_argument("--url", default="http://127.0.0.1:8081")
    ap.add_argument("--model", default="orca-local", help="a name with no steering preset")
    a = ap.parse_args()
    m = Model(a.url, a.model)

    qs = questions(m, a.n)
    print(f"{len(qs)} questions", file=sys.stderr)
    kept = dropped = 0
    with open(a.out, "w", encoding="utf-8") as f:
        for i, q in enumerate(qs):
            plain = one_line(m.ask(q, 200, system="두세 문장으로 짧게 답해. 목록이나 마크다운 없이 평범한 문장으로."))
            styled = one_line(m.ask(f"다음 글을 내용은 그대로 두고 말투만 {a.style}(으)로 바꿔 써. 바꾼 글만 출력해.\n\n"
                                    f"{plain}", 300))
            ratio = len(styled) / max(1, len(plain))
            verdict = ""
            if 0.5 <= ratio <= 2.0 and styled != plain:
                verdict = m.ask(f"글 A와 글 B가 있다.\nA: {plain}\nB: {styled}\n\n"
                                f"B가 A와 같은 내용을 담고 있고, 말투는 '{a.style}'인가? '예' 또는 '아니오'로만 답해.",
                                4, temperature=0)
            ok = verdict.startswith("예")
            print(f"[{i + 1}/{len(qs)}] {'keep' if ok else 'drop'} ratio={ratio:.2f} {verdict!r}", file=sys.stderr)
            if ok:
                f.write(f"{plain}\t{styled}\n")
                f.flush()
                kept += 1
            else:
                dropped += 1
    print(f"kept {kept}, dropped {dropped} -> {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
