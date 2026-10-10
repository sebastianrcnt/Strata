"""serve/test_reasoning_tokens.py - OpenAI usage.completion_tokens_details.reasoning_tokens: the generated tokens of
the thinking (counted as tokens, not characters), part of completion_tokens; streamed and not; 0 without thinking;
/v1/messages keeps Anthropic's usage (no such field).  Against the mock engine.

    python -m unittest serve.test_reasoning_tokens -v
"""
from __future__ import annotations

import json
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from serve.frontend import ChatTemplate  # noqa: E402
from serve.server import REASONING_WRAP_UP, ByteTokenizer, MockEngine, Service, serve  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CTX = 4096
THOUGHT = "Let me think about this. "
ANSWER = "The answer is 4."


class WordTokenizer(ByteTokenizer):
    """ByteTokenizer with one multi-character token for each word of THOUGHT and ANSWER and one for </think> (ids
    past the bytes and the specials), as the model's tokenizer has: a count of characters and of tokens differ."""
    WORDS = sorted({w for text in (THOUGHT, ANSWER) for w in text.split(" ") if len(w) > 1} | {"</think>"},
                   key=len, reverse=True)

    def __init__(self):
        super().__init__()
        self.base = 256 + len(self.SPECIALS)
        self.tokens = None

    def encode(self, text, parse_special=False):
        out, i = [], 0
        while i < len(text):
            for k, w in enumerate(self.WORDS):
                if text.startswith(w, i):
                    out.append(self.base + k)
                    i += len(w)
                    break
            else:
                special = next((s for s in self.SPECIALS if parse_special and text.startswith(s, i)), None)
                out += super().encode(special or text[i], parse_special)
                i += len(special or text[i])
        return out

    def decode(self, ids, errors="replace"):
        out, run = [], []
        for t in ids:
            if t >= self.base:
                out.append(super().decode(run, errors))
                run = []
                out.append(self.WORDS[t - self.base])
            else:
                run.append(t)
        out.append(super().decode(run, errors))
        return "".join(out)


class Scripted(MockEngine):
    """Replays `text` (special tokens parsed) and the end-of-turn token; a prompt that already ends its thinking
    (the budget's wrap-up) gets only the answer after it."""

    def __init__(self, tok, text):
        super().__init__(tok, "x", max_context=CTX)
        self.text = text

    def generate(self, ids, max_new, sampling, cancel, embeddings=None):
        done = self.tok.decode(ids).endswith("</think>\n\n")
        text = ANSWER if done else self.text
        for t in (self.tok.encode(text, parse_special=True) + self.tok.encode("<|im_end|>", parse_special=True))[:max_new]:
            if cancel.is_set():
                return
            yield t


class Base(unittest.TestCase):
    TOK = WordTokenizer
    TEXT = THOUGHT + "</think>\n\n" + ANSWER

    def setUp(self):
        self.tok = self.TOK()
        self.engine = Scripted(self.tok, self.TEXT)
        self.svc = Service(self.engine, self.tok, ChatTemplate(ROOT / "serve/chat_template.jinja"))
        self.httpd = serve(self.svc, port=0)
        self.base = f"http://127.0.0.1:{self.httpd.server_address[1]}"

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()

    def post(self, path, body):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), headers={
            "Content-Type": "application/json", "anthropic-version": "2023-06-01"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                raw = r.read().decode()
                return r.status, (json.loads(raw) if not body.get("stream") else raw)
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def openai(self, **extra):
        return self.post("/v1/chat/completions", {"model": "m", "messages": [{"role": "user", "content": "2+2?"}],
                                                  "max_tokens": 400, **extra})

    @staticmethod
    def chunks(raw):
        return [json.loads(line[6:]) for line in raw.splitlines() if line.startswith("data: {")]

    def n(self, text):
        return len(self.tok.encode(text, parse_special=True))


class OpenAI(Base):
    def test_non_stream(self):
        code, b = self.openai()
        self.assertEqual(code, 200, b)
        u = b["usage"]
        self.assertEqual(b["choices"][0]["message"]["reasoning_content"], THOUGHT)
        self.assertEqual(u["completion_tokens_details"], {"reasoning_tokens": self.n(THOUGHT)})
        # the total is unchanged: everything generated, the thinking, the answer and the end-of-turn token
        self.assertEqual(u["completion_tokens"], self.n(self.TEXT) + 1)
        self.assertEqual(u["total_tokens"], u["prompt_tokens"] + u["completion_tokens"])
        self.assertLess(u["completion_tokens_details"]["reasoning_tokens"], u["completion_tokens"])

    def test_stream_final_chunk(self):
        for extra in ({}, {"stream_options": {"include_usage": True}}):
            with self.subTest(extra=extra):
                code, raw = self.openai(stream=True, **extra)
                self.assertEqual(code, 200)
                chunks = self.chunks(raw)
                self.assertTrue(all("usage" not in c for c in chunks[:-1]))
                u = chunks[-1]["usage"]
                self.assertEqual(u["completion_tokens_details"]["reasoning_tokens"], self.n(THOUGHT))
                self.assertEqual(u["completion_tokens"], self.n(self.TEXT) + 1)
                self.assertTrue(raw.endswith("data: [DONE]\n\n"))

    def test_stream_and_whole_agree(self):
        _, whole = self.openai()
        _, raw = self.openai(stream=True)
        self.assertEqual(self.chunks(raw)[-1]["usage"], whole["usage"])

    def test_usage_key_order_is_kept(self):
        code, b = self.openai()
        self.assertEqual(list(b["usage"]), ["prompt_tokens", "completion_tokens", "total_tokens",
                                            "prompt_tokens_details", "completion_tokens_details"])

    def test_thinking_off_is_zero(self):
        for extra in ({"reasoning_effort": "none"}, {"chat_template_kwargs": {"enable_thinking": False}},
                      {"enable_thinking": False}):
            with self.subTest(extra=extra):
                self.engine.text = ANSWER
                code, b = self.openai(**extra)
                self.assertEqual(code, 200, b)
                self.assertNotIn("reasoning_content", b["choices"][0]["message"])
                self.assertEqual(b["usage"]["completion_tokens_details"], {"reasoning_tokens": 0})
                self.assertEqual(b["usage"]["completion_tokens"], self.n(ANSWER) + 1)

    def test_thinking_that_never_ends(self):
        self.engine.text = THOUGHT * 3                 # max_tokens / end of turn inside the thinking
        code, b = self.openai()
        self.assertEqual(b["usage"]["completion_tokens_details"]["reasoning_tokens"], self.n(THOUGHT * 3))
        code, b = self.openai(max_tokens=10)
        self.assertEqual(b["choices"][0]["finish_reason"], "length")
        self.assertEqual(b["usage"]["completion_tokens"], 10)
        self.assertEqual(b["usage"]["completion_tokens_details"]["reasoning_tokens"], 10)

    def test_budget_wrap_up_counts_as_thinking(self):
        code, b = self.openai(reasoning_budget_tokens=10)
        self.assertEqual(code, 200, b)
        extra = self.tok.encode(REASONING_WRAP_UP, parse_special=True)
        wrap_thinking = self.n(REASONING_WRAP_UP.split("</think>")[0])
        self.assertEqual(b["usage"]["completion_tokens_details"]["reasoning_tokens"], 10 + wrap_thinking)
        self.assertEqual(b["usage"]["completion_tokens"], 10 + len(extra) + self.n(ANSWER) + 1)

    def test_tool_call_is_not_thinking(self):
        self.engine.text = (THOUGHT + "</think>\n\n<tool_call>\n<function=get>\n<parameter=q>\nx\n</parameter>\n"
                            "</function>\n</tool_call>")
        code, b = self.openai(tools=[{"type": "function", "function": {
            "name": "get", "parameters": {"type": "object", "properties": {"q": {"type": "string"}}}}}])
        self.assertEqual(code, 200, b)
        self.assertEqual(b["choices"][0]["finish_reason"], "tool_calls")
        self.assertEqual(b["usage"]["completion_tokens_details"]["reasoning_tokens"], self.n(THOUGHT))

    def test_structured_output(self):
        self.engine.text = THOUGHT + "</think>\n\n" + '{"a": 1}'
        for stream in (False, True):
            with self.subTest(stream=stream):
                code, b = self.openai(stream=stream, response_format={"type": "json_object"})
                self.assertEqual(code, 200, b)
                u = b["usage"] if not stream else self.chunks(b)[-1]["usage"]
                self.assertEqual(u["completion_tokens_details"]["reasoning_tokens"], self.n(THOUGHT))


class Tokens(Base):
    """Multi-character tokens (and </think> one token, as in the model's tokenizer): tokens are counted, not
    characters, and the closing tag is not thinking."""

    def test_tokens_not_characters(self):
        code, b = self.openai()
        self.assertEqual(code, 200, b)
        self.assertEqual(b["choices"][0]["message"]["reasoning_content"], THOUGHT)
        got = b["usage"]["completion_tokens_details"]["reasoning_tokens"]
        self.assertEqual(got, 10)                       # 5 words + 5 spaces, of 25 characters
        self.assertLess(got, len(THOUGHT))

    def test_empty_thinking_is_zero(self):
        self.engine.text = "</think>\n\n" + ANSWER    # thinking on, but the model closes it at once
        code, b = self.openai()
        self.assertNotIn("reasoning_content", b["choices"][0]["message"])
        self.assertEqual(b["usage"]["completion_tokens_details"], {"reasoning_tokens": 0})
        self.assertEqual(b["usage"]["completion_tokens"], self.n(self.engine.text) + 1)


class ByteTokens(Base):
    """The byte tokenizer the other suites use (no </think> token): still one count per generated token."""
    TOK = ByteTokenizer

    def test_ascii_thinking(self):
        code, b = self.openai()
        self.assertEqual(code, 200, b)
        # one token per byte; the tag's first 7 bytes are held while they could still be text, so they count
        self.assertEqual(b["usage"]["completion_tokens_details"]["reasoning_tokens"], len(THOUGHT) + 7)


class Anthropic(Base):
    def test_usage_stays_anthropic(self):
        body = {"model": "m", "max_tokens": 400, "messages": [{"role": "user", "content": "2+2?"}]}
        code, b = self.post("/v1/messages", body)
        self.assertEqual(code, 200, b)
        self.assertEqual(b["content"][0]["type"], "thinking")
        self.assertEqual(set(b["usage"]), {"input_tokens", "output_tokens", "cache_read_input_tokens"})
        self.assertEqual(b["usage"]["output_tokens"], self.n(self.TEXT) + 1)
        code, raw = self.post("/v1/messages", {**body, "stream": True})
        for e in self.chunks(raw):
            usage = e.get("usage") or (e.get("message") or {}).get("usage") or {}
            self.assertNotIn("completion_tokens_details", usage)
            self.assertNotIn("reasoning_tokens", json.dumps(e))


if __name__ == "__main__":
    unittest.main()
