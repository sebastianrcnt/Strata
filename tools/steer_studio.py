#!/usr/bin/env python3
"""Steering studio: give the model a new personality from a few example sentences, try it next to the stock model,
keep it, and chat with it - a Gradio page over the strata server's /steer/* calls (serve/steering.py) and /v1.

    python tools/steer_studio.py --api http://127.0.0.1:8081 --token-file STATE/steer/admin.token \\
        --bind 127.0.0.1 --bind 100.x.y.z --port 7870

Each --bind address gets its own listener on the same app (localhost and the tailnet address, never 0.0.0.0).
Display names of the saved personalities are kept in ./labels.json (the server knows them by a short id only).
The page shows the plain steps; layers, sampling, thinking and the API names sit under "고급 설정".
"""
from __future__ import annotations

import argparse
import asyncio
import json
import re
from pathlib import Path

import gradio as gr
import httpx
import uvicorn
from fastapi import FastAPI

A = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
A.add_argument("--api", default="http://127.0.0.1:8081")
A.add_argument("--token-file", required=True)
A.add_argument("--api-key", default="", help="the strata server's API key, when it has one")
A.add_argument("--bind", action="append", default=[])
A.add_argument("--port", type=int, default=7870)
ARGS = A.parse_args()
TOKEN = Path(ARGS.token_file).read_text().strip()
HDR = {"X-Steer-Token": TOKEN, **({"Authorization": f"Bearer {ARGS.api_key}"} if ARGS.api_key else {})}
LABELS = Path("labels.json")

STOCK = "기본"
DRAFT = "만드는 중인 성격 (저장 전)"
PRESET_LABELS = {"orca-yok": "욕쟁이"}     # the server's own presets (config model_presets), by their model name
THINK = {"끔": None, "짧게": "low", "보통": "medium", "길게": "high"}

EXAMPLES = {
    "🎉 들뜬": (["와 이거 진짜 미쳤다!!! 완전 최고야, 당장 해보자!!! 🔥🔥",
                 "대박!!! 이건 무조건 성공이야, 생각만 해도 신난다!!!"],
                ["음, 괜찮은 것 같네요. 한번 해볼 수는 있겠어요.",
                 "잘될 수도 있을 것 같아요. 해보면 알겠죠."]),
    "🙄 시니컬": (["그래, 또 그 얘기냐. 어차피 안 될 거 알잖아.", "뭐, 다들 그렇게 말하더라. 결과는 늘 똑같지만."],
                  ["좋은 생각이에요. 같이 방법을 찾아봐요.", "그럴 수 있죠. 이번엔 잘될 거예요."]),
    "🏴‍☠️ 해적": (["아하르! 이 보물 지도를 보게나, 선원 녀석아!", "닻을 올려라! 오늘 밤 바다는 우리 것이다, 하하!"],
                  ["이 지도를 한번 보세요.", "이제 출발할게요. 오늘 밤에 이동합니다."]),
    "🤗 다정한": (["괜찮아, 천천히 해도 돼. 너는 이미 충분히 잘하고 있어.", "오늘 하루도 정말 고생 많았어. 푹 쉬어."],
                 ["그 정도면 됩니다. 계속 진행하세요.", "오늘 작업은 끝났습니다."]),
}

CSS = """
.gradio-container {max-width: 980px !important; margin: auto;}
#title h1 {margin-bottom: 0;}
.hint {opacity: .75; font-size: .92em;}
"""


# ---- the server
def api(method: str, path: str, body: dict | None = None, timeout: float = 600) -> dict:
    try:
        r = httpx.request(method, ARGS.api + path, json=body, headers=HDR, timeout=timeout)
    except httpx.HTTPError:
        raise gr.Error("모델 서버에 연결할 수 없어요. 잠시 후 다시 해주세요.")
    try:
        d = r.json()
    except ValueError:
        raise gr.Error(f"서버 응답 오류 ({r.status_code})")
    if r.status_code != 200:
        raise gr.Error((d.get("error") or {}).get("message") or str(d))
    return d


def status(timeout: float = 600) -> dict:
    return api("GET", "/steer/status", timeout=timeout)


def labels() -> dict:
    try:
        return json.loads(LABELS.read_text())
    except (OSError, ValueError):
        return {}


def set_label(name: str, label: str | None) -> None:
    d = labels()
    if label:
        d[name] = label
    else:
        d.pop(name, None)
    LABELS.write_text(json.dumps(d, ensure_ascii=False, indent=1))


def personas() -> list[tuple[str, str]]:
    """(display name, request key) of everything the chat can talk as."""
    out = [(STOCK, "stock")]
    try:
        presets = [m["id"] for m in httpx.get(ARGS.api + "/v1/models", headers=HDR, timeout=10).json()["data"]]
    except (httpx.HTTPError, KeyError, ValueError):
        presets = []
    out += [(PRESET_LABELS[m], m) for m in presets if m in PRESET_LABELS]
    try:
        st = status(10)
    except gr.Error:
        return out
    lab = labels()
    out += [(lab.get(d["name"], d["name"]), f"steer-{d['name']}") for d in st["saved"]]
    if "draft" in st["loaded"]:
        out.append((DRAFT, "draft"))
    return out


def request_fields(key: str) -> dict:
    if key == "stock":
        return {"experimental_speed_projection": False}
    if key == "draft":
        return {"strata_steer": "draft"}
    return {"model": key}


def stream_chat(messages, key, temperature, max_tokens, thinking):
    """Yields (reasoning, answer, stats) as the answer streams."""
    body = {"model": "local", "messages": messages, "stream": True, "max_tokens": int(max_tokens),
            "temperature": float(temperature), **request_fields(key)}
    if THINK.get(thinking) is None:
        body["enable_thinking"] = False
    else:
        body["reasoning_effort"] = THINK[thinking]
    think, text, stats = "", "", ""
    try:
        with httpx.stream("POST", ARGS.api + "/v1/chat/completions", json=body, headers=HDR, timeout=900) as r:
            if r.status_code != 200:
                r.read()
                try:
                    msg = r.json().get("error", {}).get("message")
                except ValueError:
                    msg = r.text[:300]
                raise gr.Error(f"답을 받지 못했어요: {msg}")
            for line in r.iter_lines():
                if not line.startswith("data: ") or line == "data: [DONE]":
                    continue
                c = json.loads(line[6:])
                if "error" in c:
                    raise gr.Error(c["error"].get("message", "오류"))
                for ch in c.get("choices") or []:
                    d = ch.get("delta") or {}
                    think += d.get("reasoning_content") or ""
                    text += d.get("content") or ""
                t = c.get("timings")
                if t:
                    stats = f"{t.get('predicted_n', '?')} 토큰 · 초당 {t.get('predicted_per_second', 0):.0f}"
                yield think, text, stats
    except httpx.HTTPError:
        raise gr.Error("모델 서버에 연결할 수 없어요.")


def show(think: str, text: str) -> str:
    if not think:
        return text
    return f"<details><summary>💭 생각 과정</summary>\n\n{think}\n\n</details>\n\n{text}"


def lines(t: str) -> list[str]:
    return [x.strip() for x in (t or "").splitlines() if x.strip()]


# ---- make
def make(pos_t, neg_t, strength, first, last):
    pos, neg = lines(pos_t), lines(neg_t)
    if not pos or not neg:
        raise gr.Error("양쪽에 문장을 하나 이상 적어주세요.")
    if len(pos) != len(neg):
        gr.Warning(f"왼쪽 {len(pos)}줄, 오른쪽 {len(neg)}줄이에요. 같은 내용끼리 줄을 맞추면 더 정확해요.")
    d = api("POST", "/steer/measure", {"pos": pos, "neg": neg, "scale": strength / 100,
                                       "first": int(first), "last": int(last)})
    plot = {"층": list(range(len(d["ratios"]))), "차이": d["ratios"]}
    import pandas as pd
    return (f"✅ 성격을 만들었어요 ({d['seconds']}초). 아래에서 미리보기로 확인해 보세요.",
            pd.DataFrame(plot), gr.update(visible=True))


def preview(question, strength, first, last, temperature, max_tokens, thinking):
    if not question.strip():
        raise gr.Error("질문을 적어주세요.")
    api("POST", "/steer/draft", {"scale": strength / 100, "first": int(first), "last": int(last)})
    msgs = [{"role": "user", "content": question}]
    a, b = "", ""
    for think, text, _ in stream_chat(msgs, "stock", temperature, max_tokens, thinking):
        a = show(think, text)
        yield a, b
    for think, text, _ in stream_chat(msgs, "draft", temperature, max_tokens, thinking):
        b = show(think, text)
        yield a, b
    if not b.strip():
        gr.Warning("새 성격이 아무 말도 못 했어요. 세기를 낮춰 보세요.")


def slug(label: str, taken: set[str]) -> str:
    base = re.sub(r"[^a-z0-9]+", "-", label.lower()).strip("-")[:24] or "persona"
    name, i = base, 2
    while name in taken:
        name, i = f"{base}-{i}", i + 1
    return name


def save(label, strength, first, last, api_name):
    label = (label or "").strip()
    if not label:
        raise gr.Error("이름을 적어주세요.")
    lab = labels()
    by_label = {v: k for k, v in lab.items()}
    name = (api_name or "").strip() or by_label.get(label) or slug(label, set(lab) | {"draft"})
    d = api("POST", "/steer/save", {"name": name, "scale": strength / 100, "first": int(first), "last": int(last)})
    set_label(d["name"], label)
    choices = personas()
    return (f"💾 **{label}** 저장 완료! 대화 탭에서 골라 쓸 수 있어요. (API 모델 이름: `{d['model']}`)",
            gr.update(choices=choices, value=f"steer-{d['name']}"), gr.update(choices=saved_choices()))


# ---- mine
def saved_choices() -> list[tuple[str, str]]:
    try:
        st = status(10)
    except gr.Error:
        return []
    lab = labels()
    return [(lab.get(d["name"], d["name"]), d["name"]) for d in st["saved"]]


def pick(name):
    if not name:
        return gr.update(), gr.update(), gr.update(), "", ""
    d = next((x for x in status(10)["saved"] if x["name"] == name), None)
    if d is None:
        return gr.update(), gr.update(), gr.update(), "", ""
    pairs = d.get("pairs") or {}
    ex = "\n".join(f"- {p}" for p in pairs.get("pos", [])[:5])
    return (round(d["scale"] * 100, 2), d["first"], d["last"], f"**이런 말투로 만들었어요**\n{ex}",
            f"API 모델 이름: `steer-{name}` · 층 {d['first']}-{d['last']} · 세기 {d['scale'] * 100:g}%")


def update(name, strength, first, last):
    if not name:
        raise gr.Error("성격을 골라주세요.")
    api("POST", "/steer/update", {"name": name, "scale": strength / 100, "first": int(first), "last": int(last)})
    return f"✅ 세기를 {strength:g}로 바꿨어요."


def delete(name):
    if not name:
        raise gr.Error("성격을 골라주세요.")
    api("POST", "/steer/delete", {"name": name})
    set_label(name, None)
    return "🗑️ 삭제했어요.", gr.update(choices=saved_choices(), value=None), gr.update(choices=personas(), value="stock")


# ---- chat
def chat_send(msg, hist, key, system, temperature, max_tokens, thinking):
    """hist: [{role, content (what the model saw), shown (what the page shows)}]."""
    view = lambda h: [{"role": m["role"], "content": m.get("shown", m["content"])} for m in h]   # noqa: E731
    if not msg.strip():
        yield "", view(hist), hist, ""
        return
    hist = hist + [{"role": "user", "content": msg}]
    msgs = ([{"role": "system", "content": system}] if system.strip() else [])
    msgs += [{"role": m["role"], "content": m["content"]} for m in hist]
    text, stats, shown = "", "", ""
    for think, text, stats in stream_chat(msgs, key, temperature, max_tokens, thinking):
        shown = show(think, text)
        yield "", view(hist) + [{"role": "assistant", "content": shown}], hist, ""
    hist = hist + [{"role": "assistant", "content": text, "shown": shown}]
    yield "", view(hist), hist, stats


def build():
    try:
        cover = status(10).get("cover") or [1, 47]
    except gr.Error:
        cover = [1, 47]
    lo, hi = cover
    with gr.Blocks(title="성격 스튜디오", theme=gr.themes.Soft(primary_hue="violet"), css=CSS) as demo:
        gr.Markdown("# 🎭 성격 스튜디오\n예시 문장 몇 개로 AI에게 새 성격을 입혀 보세요.", elem_id="title")

        with gr.Tab("💬 대화"):
            with gr.Row():
                c_who = gr.Dropdown(personas(), value="stock", label="누구랑 대화할까요?", scale=4)
                c_new = gr.Button("🧹 새 대화", scale=1)
            c_bot = gr.Chatbot(type="messages", height=480, show_label=False, render_markdown=True,
                               placeholder="성격을 고르고 말을 걸어 보세요.")
            with gr.Row():
                c_in = gr.Textbox(placeholder="메시지를 입력하고 Enter", show_label=False, scale=6, autofocus=True)
                c_send = gr.Button("보내기", variant="primary", scale=1)
            c_stats = gr.Markdown(elem_classes="hint")
            with gr.Accordion("고급 설정", open=False):
                c_sys = gr.Textbox(label="시스템 프롬프트", lines=2, placeholder="(선택) 역할이나 규칙")
                with gr.Row():
                    c_think = gr.Radio(list(THINK), value="끔", label="생각 모드")
                    c_temp = gr.Slider(0, 1.5, value=0.7, step=0.05, label="창의성 (temperature)")
                    c_max = gr.Slider(64, 8192, value=1024, step=64, label="최대 길이 (토큰)")
            hist = gr.State([])
            ins = [c_in, hist, c_who, c_sys, c_temp, c_max, c_think]
            outs = [c_in, c_bot, hist, c_stats]
            c_in.submit(chat_send, ins, outs)
            c_send.click(chat_send, ins, outs)
            c_new.click(lambda: ([], [], ""), None, [c_bot, hist, c_stats])
            c_who.change(lambda: ([], [], ""), None, [c_bot, hist, c_stats])

        with gr.Tab("✨ 새 성격 만들기"):
            gr.Markdown("**1. 예시 문장 적기** — 왼쪽엔 원하는 말투로, 오른쪽엔 같은 내용을 평범하게. "
                        "한 줄에 한 문장, 같은 줄끼리 짝이에요. 한 쌍만 있어도 돼요.")
            with gr.Row():
                ex_btns = [gr.Button(k, size="sm") for k in EXAMPLES]
            with gr.Row():
                pos = gr.Textbox(label="이 성격이라면 이렇게 말해요", lines=5,
                                 placeholder="와 이거 진짜 미쳤다!!! 당장 해보자!!!")
                neg = gr.Textbox(label="평소엔 이렇게 말해요", lines=5,
                                 placeholder="괜찮은 것 같네요. 한번 해볼게요.")
            gr.Markdown("**2. 세기 정하고 만들기**")
            strength = gr.Slider(0.5, 6, value=2.5, step=0.1, label="세기",
                                 info="약하게 ← → 강하게 · 너무 세면 말이 이상해져요 (보통 2~3.5)")
            make_btn = gr.Button("✨ 성격 만들기", variant="primary")
            made = gr.Markdown()
            with gr.Group(visible=False) as after:
                gr.Markdown("**3. 미리보기** — 같은 질문에 기본과 새 성격이 어떻게 다르게 답하는지 보세요. "
                            "세기를 바꾸고 다시 눌러도 돼요.")
                q = gr.Textbox(label="질문", value="주말에 뭐 하면 좋을까?")
                prev_btn = gr.Button("👀 미리보기", variant="primary")
                with gr.Row():
                    with gr.Column():
                        gr.Markdown("#### 기본")
                        out_a = gr.Markdown()
                    with gr.Column():
                        gr.Markdown("#### 새 성격")
                        out_b = gr.Markdown()
                gr.Markdown("**4. 저장**")
                with gr.Row():
                    label = gr.Textbox(label="성격 이름", placeholder="예: 들뜬 친구", scale=4)
                    save_btn = gr.Button("💾 저장", variant="primary", scale=1)
                saved_msg = gr.Markdown()
            with gr.Accordion("고급 설정", open=False):
                with gr.Row():
                    first = gr.Slider(lo, hi, value=8, step=1, label="적용 시작 층")
                    last = gr.Slider(lo, hi, value=32, step=1, label="적용 끝 층")
                with gr.Row():
                    p_think = gr.Radio(list(THINK), value="끔", label="미리보기 생각 모드")
                    p_temp = gr.Slider(0, 1.5, value=0.7, step=0.05, label="미리보기 temperature")
                    p_max = gr.Slider(64, 4096, value=400, step=32, label="미리보기 최대 길이")
                api_name = gr.Textbox(label="API 이름 (비우면 자동, a-z 0-9 _ -)",
                                      placeholder="hype → 모델 이름 steer-hype")
                plot = gr.LinePlot(x="층", y="차이", title="층별 차이 (차이 / 평균 크기)", height=200)

        with gr.Tab("📚 내 성격들"):
            m_pick = gr.Dropdown(saved_choices(), label="성격 고르기", value=None)
            m_info = gr.Markdown()
            m_strength = gr.Slider(0.5, 6, value=2.5, step=0.1, label="세기", info="약하게 ← → 강하게")
            with gr.Row():
                m_upd = gr.Button("✅ 세기 저장", variant="primary")
                m_del = gr.Button("🗑️ 삭제", variant="stop")
            m_msg = gr.Markdown()
            with gr.Accordion("고급 설정", open=False):
                m_api = gr.Markdown()
                with gr.Row():
                    m_first = gr.Slider(lo, hi, value=8, step=1, label="적용 시작 층")
                    m_last = gr.Slider(lo, hi, value=32, step=1, label="적용 끝 층")

        for b in ex_btns:
            b.click(lambda k=b.value: ("\n".join(EXAMPLES[k][0]), "\n".join(EXAMPLES[k][1])), None, [pos, neg])
        make_btn.click(make, [pos, neg, strength, first, last], [made, plot, after])
        prev_btn.click(preview, [q, strength, first, last, p_temp, p_max, p_think], [out_a, out_b])
        save_btn.click(save, [label, strength, first, last, api_name], [saved_msg, c_who, m_pick])
        m_pick.change(pick, m_pick, [m_strength, m_first, m_last, m_info, m_api])
        m_upd.click(update, [m_pick, m_strength, m_first, m_last], m_msg)
        m_del.click(delete, m_pick, [m_msg, m_pick, c_who])

        def on_load():
            """Each page load: the server's lists now, and the draft being made (it may have restarted since)."""
            try:
                d = status(10).get("draft") or {}
            except gr.Error:
                d = {}
            p = d.get("pairs") or {}
            keep = gr.update()
            return (gr.update(choices=personas()), gr.update(choices=saved_choices()),
                    "\n".join(p.get("pos", [])) or keep, "\n".join(p.get("neg", [])) or keep,
                    round(float(d["scale"]) * 100, 2) if "scale" in d else keep,
                    int(d["first"]) if "first" in d else keep, int(d["last"]) if "last" in d else keep,
                    gr.update(visible=bool(d)))

        demo.load(on_load, None, [c_who, m_pick, pos, neg, strength, first, last, after])
    return demo


async def main():
    demo = build().queue(default_concurrency_limit=1)
    app = gr.mount_gradio_app(FastAPI(), demo, path="/")
    binds = ARGS.bind or ["127.0.0.1"]
    if any(b in ("0.0.0.0", "::") for b in binds):
        raise SystemExit("bind localhost and the tailnet address, not every interface")
    servers = [uvicorn.Server(uvicorn.Config(app, host=b, port=ARGS.port, log_level="warning")) for b in binds]
    print(f"steer studio: {', '.join(f'http://{b}:{ARGS.port}/' for b in binds)}", flush=True)
    await asyncio.gather(*(s.serve() for s in servers))


if __name__ == "__main__":
    asyncio.run(main())
