"""serve/test_stop.py - stop sequences, explicit 400s for parameters Strata cannot honour, and the thinking-budget
aliases, over both APIs (stream and not), against the mock engine (no GPU, no pack).

    python -m unittest serve.test_stop -v
"""
from __future__ import annotations

import json
import random
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from serve.frontend import ChatTemplate, Event, anthropic_to_messages, openai_to_messages  # noqa: E402
from serve.server import REASONING_WRAP_UP, ByteTokenizer, MockEngine, Service, StopFilter, serve  # noqa: E402
from serve.test_server import ThinkingEngine  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
CTX = 4096
PRE = "hm</think>\n\n"                       # a little thinking, then the answer (the template thinks by default)
APIS = ("openai", "anthropic")
TOOLS = {"openai": [{"type": "function", "function": {"name": "write", "description": "write a file",
                                                      "parameters": {"type": "object", "properties": {"path": {"type": "string"}}}}}],
         "anthropic": [{"name": "write", "description": "write a file",
                        "input_schema": {"type": "object", "properties": {"path": {"type": "string"}}}}]}
CALL = "<tool_call>\n<function=write>\n<parameter=path>\nEND.txt\n</parameter>\n</function>\n</tool_call>"


class CountingEngine(MockEngine):
    """Says what it is told to and counts the tokens it handed out: fewer than the script's `total` means the
    generator was closed early, which is what makes StrataEngine send STOP and drain."""

    def __init__(self, tok):
        super().__init__(tok, "x", max_context=CTX)
        self.sent, self.total, self.prompts = 0, 0, []

    def say(self, text):
        self.script = self.tok.encode(text, parse_special=True) + self.tok.encode("<|im_end|>", parse_special=True)
        self.scripts = [self.script]
        self.total = len(self.script)
        return self.total

    def generate(self, ids, max_new, sampling, cancel, embeddings=None):
        self.prompts.append(list(ids))
        self.sent = 0
        for t in super().generate(ids, max_new, sampling, cancel, embeddings):
            self.sent += 1
            yield t


class Reply:
    """One response, whatever the API and stream or not."""

    def __init__(self, api, stream, status, headers, raw):
        self.api, self.stream, self.status, self.rid = api, stream, status, headers.get("X-Request-Id")
        self.content = self.reasoning = ""
        self.finish = self.stop_sequence = self.error = None
        self.completion_tokens, self.deltas, self.calls = None, [], []
        if status != 200:
            self.error = json.loads(raw)["error"]
            return
        if api == "openai":
            if stream:
                chunks = [json.loads(line[6:]) for line in raw.splitlines() if line.startswith("data: {")]
                by_index = {}
                for c in chunks:
                    ch = c["choices"][0]
                    d = ch["delta"]
                    if d.get("content"):
                        self.deltas.append(d["content"])
                    self.reasoning += d.get("reasoning_content") or ""
                    for tc in d.get("tool_calls") or []:
                        cur = by_index.setdefault(tc["index"], {"name": "", "args": ""})
                        cur["name"] += (tc.get("function") or {}).get("name") or ""
                        cur["args"] += (tc.get("function") or {}).get("arguments") or ""
                    self.finish = ch["finish_reason"] or self.finish
                    if c.get("usage"):
                        self.completion_tokens = c["usage"]["completion_tokens"]
                self.content = "".join(self.deltas)
                self.calls = [(v["name"], json.loads(v["args"])) for _, v in sorted(by_index.items())]
            else:
                b = json.loads(raw)
                m = b["choices"][0]["message"]
                self.content, self.reasoning = m.get("content") or "", m.get("reasoning_content") or ""
                self.calls = [(c["function"]["name"], json.loads(c["function"]["arguments"])) for c in m.get("tool_calls", [])]
                self.finish, self.completion_tokens = b["choices"][0]["finish_reason"], b["usage"]["completion_tokens"]
        else:
            if stream:
                evs = [json.loads(line[6:]) for line in raw.splitlines() if line.startswith("data: {")]
                blocks = {}
                for e in evs:
                    if e["type"] == "content_block_start":
                        blocks[e["index"]] = {"b": e["content_block"], "json": ""}
                    elif e["type"] == "content_block_delta":
                        d = e["delta"]
                        if d["type"] == "text_delta":
                            self.deltas.append(d["text"])
                        elif d["type"] == "thinking_delta":
                            self.reasoning += d["thinking"]
                        else:
                            blocks[e["index"]]["json"] += d["partial_json"]
                    elif e["type"] == "message_delta":
                        self.finish, self.stop_sequence = e["delta"]["stop_reason"], e["delta"]["stop_sequence"]
                        self.completion_tokens = e["usage"]["output_tokens"]
                self.content = "".join(self.deltas)
                self.calls = [(v["b"]["name"], json.loads(v["json"] or "{}")) for _, v in sorted(blocks.items())
                              if v["b"]["type"] == "tool_use"]
            else:
                b = json.loads(raw)
                self.content = "".join(x.get("text", "") for x in b["content"] if x["type"] == "text")
                self.reasoning = "".join(x.get("thinking", "") for x in b["content"] if x["type"] == "thinking")
                self.calls = [(x["name"], x["input"]) for x in b["content"] if x["type"] == "tool_use"]
                self.finish, self.stop_sequence = b["stop_reason"], b["stop_sequence"]
                self.completion_tokens = b["usage"]["output_tokens"]


class Base(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tok = ByteTokenizer()
        cls.engine = CountingEngine(tok)
        cls.svc = Service(cls.engine, tok, ChatTemplate(ROOT / "serve/chat_template.jinja"))
        cls.svc.api_monitor = True
        cls.httpd = serve(cls.svc, port=0)
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=30) as r:
            return json.loads(r.read())

    def post_raw(self, path, body):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), headers={
            "Content-Type": "application/json", "anthropic-version": "2023-06-01"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.headers, r.read().decode()
        except urllib.error.HTTPError as e:
            with e:
                return e.code, e.headers, e.read().decode()

    def call(self, api, stream, text=None, **extra):
        if text is not None:
            self.engine.say(text)
        body = {"model": "m", "max_tokens": 2000, "stream": stream, "messages": [{"role": "user", "content": "hi"}]}
        body.update(extra)
        path = "/v1/chat/completions" if api == "openai" else "/v1/messages"
        return Reply(api, stream, *self.post_raw(path, body))

    @staticmethod
    def stops(api, *strings):
        return {"stop": list(strings)} if api == "openai" else {"stop_sequences": list(strings)}

    def each(self):
        for api in APIS:
            for stream in (False, True):
                with self.subTest(api=api, stream=stream):
                    yield api, stream


class StopSequences(Base):
    def test_stop_halts_the_engine_and_hides_the_string(self):
        for api, stream in self.each():
            total = self.engine.say(PRE + "alpha beta END gamma " + "z" * 800)
            before = self.get("/metrics")["totals"]
            r = self.call(api, stream, **self.stops(api, "END"))
            want = len(PRE + "alpha beta END")                        # tokens up to the one that completed the string
            self.assertEqual((r.status, r.content, r.reasoning), (200, "alpha beta ", "hm"))
            self.assertEqual(r.finish, "stop" if api == "openai" else "stop_sequence")
            self.assertEqual(r.stop_sequence, None if api == "openai" else "END")
            self.assertEqual(self.engine.sent, want)                      # STOP, not decoding on to 2000
            self.assertLess(self.engine.sent, total // 10)
            self.assertEqual(r.completion_tokens, want)               # what was generated, as for a cancel
            u = self.get("/v1/usage/" + r.rid)
            self.assertEqual((u["state"], u["finish_reason"], u["engine_finish"], u["completion_tokens"]),
                             ("done", r.finish, "stop", want))
            m = self.get("/metrics")
            self.assertEqual(m["requests"][0]["finish"], "stop")      # a normal end, not "disconnect"/"cancel"
            self.assertEqual(m["totals"]["requests"] - before["requests"], 1)
            self.assertEqual(m["totals"]["output_tokens"] - before["output_tokens"], want)
            rec = self.get("/api/requests?id=" + r.rid)
            self.assertEqual((rec.get("state"), rec.get("outcome")), ("completed", None))

    def test_a_stop_in_the_very_first_token(self):
        for api, stream in self.each():
            off = {"chat_template_kwargs": {"enable_thinking": False}} if api == "openai" else \
                {"thinking": {"type": "disabled"}}
            r = self.call(api, stream, "#never shown " + "z" * 500, **off, **self.stops(api, "#"))
            self.assertEqual((r.content, self.engine.sent), ("", 1))
            self.assertLess(self.engine.sent, self.engine.total)
            self.assertEqual(r.finish, "stop" if api == "openai" else "stop_sequence")
            self.assertEqual(r.completion_tokens, 1)
            self.assertEqual(self.get("/v1/usage/" + r.rid)["state"], "done")

    def test_a_held_back_prefix_that_does_not_match_is_flushed(self):
        for api, stream in self.each():
            text = "go EN then E and EN"
            r = self.call(api, stream, PRE + text, **self.stops(api, "END"))
            self.assertEqual(r.content, text)                           # nothing lost, nothing added
            self.assertEqual(r.finish, "stop" if api == "openai" else "end_turn")
            self.assertIsNone(r.stop_sequence)
            self.assertEqual(self.engine.sent, self.engine.total)       # ran to its own end
            if stream:
                # never a partial stop string on its own before the next text shows what it is; the tail "EN" is
                # only let go at the end of the generation
                self.assertEqual(r.deltas[-1], "EN")
                self.assertNotIn("E", [d for d in r.deltas[:-1]])
                self.assertFalse([d for d in r.deltas[:-1] if d.endswith(("E", "EN"))], r.deltas)

    def test_a_string_split_across_chunks_never_leaks(self):
        for api, stream in self.each():
            r = self.call(api, stream, PRE + "say ENDING now " + "z" * 200, **self.stops(api, "END"))
            self.assertEqual(r.content, "say ")
            if stream:
                self.assertFalse([d for d in r.deltas if "E" in d or "N" in d or "D" in d], r.deltas)

    def test_multibyte(self):
        cases = [("你好世界再见", "世界", "你好"), ("ok😀more", "😀", "ok"), ("abécd", "éc", "ab"),
                 ("你好世界", "世X", "你好世界"), ("你好世界", "好世界再", "你好世界")]
        for api, stream in self.each():
            for text, stop, want in cases:
                r = self.call(api, stream, PRE + text, **self.stops(api, stop))
                self.assertEqual(r.content, want, (text, stop))
                self.assertNotIn("�", r.content)
                if want != text:
                    self.assertEqual(self.engine.sent, len((PRE + text[:len(want)] + stop).encode()))

    def test_overlapping_strings_the_first_to_complete_wins_whatever_the_chunking(self):
        # ("abc" completes before "bcd"); a tie on the end goes to the longer one
        for api, stream in self.each():
            for text, stops, want, hit in (("xabcdy", ("abc", "bcd"), "x", "abc"), ("xabcdy", ("bcd", "abc"), "x", "abc"),
                                           ("abcd", ("cd", "bcd"), "a", "bcd"), ("xab", ("b", "ab"), "x", "ab"),
                                           ("xabcdy", ("abcd", "c"), "xab", "c")):
                r = self.call(api, stream, PRE + text, **self.stops(api, *stops))
                self.assertEqual(r.content, want, (text, stops))
                if api == "anthropic":
                    self.assertEqual(r.stop_sequence, hit)

    def test_the_reasoning_to_answer_boundary_does_not_match(self):
        for api, stream in self.each():
            # "EN" ends the thinking, "D" starts the answer
            r = self.call(api, stream, "think EN</think>\n\nD tail", **self.stops(api, "END"))
            self.assertEqual((r.content, r.reasoning), ("D tail", "think EN"))
            self.assertEqual(self.engine.sent, self.engine.total)       # ran to its own end
            # the newlines the parser drops after </think> are not part of the answer either
            r = self.call(api, stream, "t</think>\n\nanswer", **self.stops(api, "\n\nan", "t</think>"))
            self.assertEqual((r.content, r.reasoning), ("answer", "t"))
            self.assertEqual(self.engine.sent, self.engine.total)       # ran to its own end
            # the whole string inside the thinking: the thinking is not searched
            r = self.call(api, stream, "an END here</think>\n\nanswer", **self.stops(api, "END"))
            self.assertEqual((r.content, r.reasoning), ("answer", "an END here"))
            self.assertEqual(self.engine.sent, self.engine.total)       # ran to its own end

    def test_tool_calls_are_not_searched_and_a_held_prefix_comes_first(self):
        for api, stream in self.each():
            r = self.call(api, stream, PRE + "Sure E" + CALL, tools=TOOLS[api], **self.stops(api, "END"))
            self.assertEqual(r.calls, [("write", {"path": "END.txt"})])   # the call is whole, its "END" intact
            self.assertEqual(r.content, "Sure E")
            self.assertEqual(r.finish, "tool_calls" if api == "openai" else "tool_use")
            self.assertIsNone(r.stop_sequence)
            self.assertEqual(self.engine.sent, self.engine.total)       # ran to its own end

    def test_a_stop_in_the_text_before_a_tool_call_ends_it_there(self):
        for api, stream in self.each():
            r = self.call(api, stream, PRE + "abc END " + CALL, tools=TOOLS[api], **self.stops(api, "END"))
            self.assertEqual((r.content, r.calls), ("abc ", []))
            self.assertEqual(r.finish, "stop" if api == "openai" else "stop_sequence")
            self.assertLess(self.engine.sent, self.engine.total)

    def test_a_string_sent_alone_and_up_to_four_and_none(self):
        for api, stream in self.each():
            if api == "openai":
                r = self.call(api, stream, PRE + "one two", stop="two")
                self.assertEqual(r.content, "one ")
                r = self.call(api, stream, PRE + "one two", stop=["a", "b", "c", "two"])
                self.assertEqual(r.content, "one ")
            for none in ([], None):
                r = self.call(api, stream, PRE + "one two", **{"stop" if api == "openai" else "stop_sequences": none})
                self.assertEqual((r.status, r.content), (200, "one two"))
            # no stop strings: as before (a finish "length" stays "length")
            r = self.call(api, stream, PRE + "x" * 50, max_tokens=len(PRE) + 10)
            self.assertEqual(r.finish, "length" if api == "openai" else "max_tokens")

    def test_a_match_only_in_the_text_the_parser_held_to_the_end(self):
        for api, stream in self.each():     # the parser keeps trailing newlines back until a character follows
            r = self.call(api, stream, PRE + "Done.\n\n", **self.stops(api, "\n\n"))
            self.assertEqual(r.content, "Done.")
            self.assertEqual(r.finish, "stop" if api == "openai" else "stop_sequence")

    def test_a_bad_stop_is_a_400_in_the_error_format(self):
        bad = {"openai": [123, {"a": 1}, [1], ["a", 2], "", [""], ["a", ""], list("abcde"), True],
               "anthropic": ["x", 5, {"a": 1}, [1], [""], ["a", None], ["ok", ""]]}
        for api in APIS:
            key = "stop" if api == "openai" else "stop_sequences"
            for stream in (False, True):
                for value in bad[api]:
                    with self.subTest(api=api, stream=stream, value=value):
                        before = len(self.engine.prompts)
                        r = self.call(api, stream, **{key: value})
                        self.assertEqual(r.status, 400, value)
                        self.assertEqual(r.error["type"], "invalid_request_error")
                        self.assertIn(key, r.error["message"])
                        self.assertEqual(len(self.engine.prompts), before)     # never reached the engine
                        self.assertEqual(self.get("/v1/usage/" + r.rid)["state"], "error")
        # more than four is only OpenAI's limit
        self.engine.say(PRE + "a b c d e f")
        r = self.call("anthropic", False, **self.stops("anthropic", *"12345"))
        self.assertEqual(r.status, 200)

    def test_filter_is_chunking_independent(self):
        rng = random.Random(7)
        for _ in range(3000):
            alphabet = "ab\n世"
            stops = list(dict.fromkeys("".join(rng.choice(alphabet) for _ in range(rng.randint(1, 4))) for _ in range(rng.randint(1, 3))))
            text = "".join(rng.choice(alphabet) for _ in range(rng.randint(0, 25)))
            # one character at a time is the reference: the first stop string to complete
            ref = StopFilter(stops)
            out = ""
            for ch in text:
                out += "".join(e.text for e in ref.feed([Event("content", ch)]))
                if ref.hit:
                    break
            if not ref.hit:
                out += "".join(e.text for e in ref.flush())
            cuts = sorted(rng.sample(range(len(text) + 1), min(len(text) + 1, rng.randint(0, 4))))
            pieces = [text[a:b] for a, b in zip([0] + cuts, cuts + [len(text)])]
            f = StopFilter(stops)
            got = ""
            for p in pieces:
                got += "".join(e.text for e in f.feed([Event("content", p)] if p else []))
                if f.hit:
                    break
            if not f.hit:
                got += "".join(e.text for e in f.flush())
            self.assertEqual((got, f.hit), (out, ref.hit), (stops, text, pieces))
            if f.hit:
                self.assertTrue(text.startswith(got + f.hit))
                self.assertNotIn(f.hit, got)

    def test_filter_passes_other_events_in_order(self):
        f = StopFilter(["END"])
        out = f.feed([Event("reasoning", "an END"), Event("content", "ab E"), Event("tool_start", call=None),
                      Event("content", "EN"), Event("content", "D!"), Event("content", "never")])
        self.assertEqual([(e.kind, e.text) for e in out],
                         [("reasoning", "an END"), ("content", "ab "), ("content", "E"), ("tool_start", "")])
        self.assertEqual((f.hit, f.flush()), ("END", []))


class Unsupported(Base):
    def test_each_unsupported_parameter_is_a_400(self):
        bad = {"openai": [("n", {"n": 2}), ("n", {"n": 0}), ("n", {"n": "1"}), ("n", {"n": True}),
                          ("logprobs", {"logprobs": True}), ("top_logprobs", {"top_logprobs": 3}),
                          ("top_logprobs", {"top_logprobs": 0}), ("top_logprobs", {"logprobs": True, "top_logprobs": 2}),
                          ("tool_choice", {"tool_choice": "required"}),
                          ("tool_choice", {"tool_choice": {"type": "function", "function": {"name": "write"}}}),
                          ("tool_choice", {"tool_choice": "bogus"}),
                          ("parallel_tool_calls", {"parallel_tool_calls": False})],
               "anthropic": [("tool_choice", {"tool_choice": {"type": "any"}}),
                             ("tool_choice", {"tool_choice": {"type": "tool", "name": "write"}}),
                             ("disable_parallel_tool_use", {"tool_choice": {"type": "auto", "disable_parallel_tool_use": True}})]}
        for api in APIS:
            for stream in (False, True):
                for word, extra in bad[api]:
                    with self.subTest(api=api, stream=stream, extra=extra):
                        before = len(self.engine.prompts)
                        r = self.call(api, stream, tools=TOOLS[api], **extra)
                        self.assertEqual(r.status, 400)
                        self.assertEqual(r.error["type"], "invalid_request_error")
                        self.assertIn(word, r.error["message"])
                        self.assertEqual(len(self.engine.prompts), before)
                        self.assertEqual(self.get("/v1/usage/" + r.rid)["state"], "error")

    def test_what_is_allowed_and_the_harmless_extras(self):
        ok = {"openai": [{}, {"n": None}, {"n": 1}, {"logprobs": False}, {"logprobs": None}, {"top_logprobs": None},
                         {"tool_choice": None}, {"tool_choice": "auto"}, {"tool_choice": "none"},
                         {"parallel_tool_calls": True}, {"parallel_tool_calls": None},
                         {"user": "u", "metadata": {"a": 1}, "store": False, "stream_options": {"include_usage": True},
                          "service_tier": "auto", "seed": 3, "frequency_penalty": 0.1, "made_up": [1]}],
              "anthropic": [{}, {"tool_choice": {"type": "auto"}}, {"tool_choice": {"type": "none"}},
                            {"tool_choice": {"type": "auto", "disable_parallel_tool_use": False}},
                            {"metadata": {"user_id": "u"}, "top_k": 5, "service_tier": "auto", "made_up": 1,
                             "n": 3, "logprobs": True}]}      # (n/logprobs are no Anthropic fields: left alone there)
        for api in APIS:
            for stream in (False, True):
                for extra in ok[api]:
                    with self.subTest(api=api, stream=stream, extra=extra):
                        r = self.call(api, stream, PRE + "fine", tools=TOOLS[api], **extra)
                        self.assertEqual((r.status, r.content), (200, "fine"), r.error)

    def test_tool_choice_none_does_not_offer_the_tools(self):
        for api in APIS:
            none = "none" if api == "openai" else {"type": "none"}
            auto = "auto" if api == "openai" else {"type": "auto"}
            self.call(api, False, PRE + "x", tools=TOOLS[api], tool_choice=auto)
            offered = self.tok_decode(self.engine.prompts[-1])
            self.call(api, False, PRE + "x", tools=TOOLS[api], tool_choice=none)
            hidden = self.tok_decode(self.engine.prompts[-1])
            self.call(api, False, PRE + "x")
            plain = self.tok_decode(self.engine.prompts[-1])
            with self.subTest(api=api):
                self.assertIn("write a file", offered)
                self.assertNotIn("write a file", hidden)
                self.assertNotIn('"write"', hidden)
                self.assertEqual(hidden, plain)                 # exactly the prompt of a request with no tools

    @staticmethod
    def tok_decode(ids):
        return ByteTokenizer().decode(ids)

    def test_count_tokens_follows_tool_choice_none(self):
        base = {"model": "m", "messages": [{"role": "user", "content": "hi"}]}
        def count(**extra):
            s, _, raw = self.post_raw("/v1/messages/count_tokens", {**base, **extra})
            self.assertEqual(s, 200, raw)
            return json.loads(raw)["input_tokens"]
        self.assertEqual(count(tools=TOOLS["anthropic"], tool_choice={"type": "none"}), count())
        self.assertGreater(count(tools=TOOLS["anthropic"], tool_choice={"type": "auto"}), count())


class BudgetAliases(unittest.TestCase):
    """thinking_budget (Qwen), thinking {"type": "enabled", "budget_tokens"} (Anthropic) and reasoning.max_tokens
    (OpenRouter) set the thinking cap like reasoning_budget_tokens; Qwen's enable_thinking=false turns it off."""

    def setUp(self):
        self.tok = ByteTokenizer()
        self.engine = ThinkingEngine(self.tok)
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
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def openai(self, **extra):
        return self.post("/v1/chat/completions", {"model": "m", "messages": [{"role": "user", "content": "2+2?"}],
                                                  "max_tokens": 400, **extra})

    def anthropic(self, **extra):
        return self.post("/v1/messages", {"model": "m", "messages": [{"role": "user", "content": "2+2?"}],
                                          "max_tokens": 400, **extra})

    ALIASES = {"thinking_budget": lambda n: {"thinking_budget": n},
               "thinking.budget_tokens": lambda n: {"thinking": {"type": "enabled", "budget_tokens": n}},
               "reasoning.max_tokens": lambda n: {"reasoning": {"max_tokens": n}}}

    def test_each_alias_sets_the_cap(self):
        rb = self.svc.reasoning_budget
        self.assertEqual(rb({"thinking_budget": 33}), 33)
        self.assertEqual(rb({"thinking": {"type": "enabled", "budget_tokens": 44}}), 44)
        self.assertEqual(rb({"reasoning": {"max_tokens": 55}}), 55)
        self.assertEqual(rb({"reasoning_budget_tokens": 66}), 66)
        for name, make in self.ALIASES.items():
            self.assertIsNone(rb(make(0)), name)                    # 0: no cap
            self.assertEqual(rb(make(12.0)), 12, name)              # a whole number, however JSON spells it

    def test_disabled_thinking_gives_no_cap(self):
        rb = self.svc.reasoning_budget
        self.assertIsNone(rb({"thinking": {"type": "disabled", "budget_tokens": 44}}))
        self.assertEqual(rb({"thinking": {"type": "disabled", "budget_tokens": 44}, "thinking_budget": 9}), 9)
        self.assertIsNone(rb({"thinking": {"budget_tokens": 44}}))   # no type: not "enabled"

    def test_precedence(self):
        rb = self.svc.reasoning_budget
        every = {"reasoning_budget_tokens": 1, "thinking_budget": 2, "thinking": {"type": "enabled", "budget_tokens": 3},
                 "reasoning": {"max_tokens": 4, "effort": "low"}, "output_config": {"effort": "high"}}
        self.assertEqual(rb(every), 1)
        del every["reasoning_budget_tokens"]
        self.assertEqual(rb(every), 2)
        del every["thinking_budget"]
        self.assertEqual(rb(every), 3)
        del every["thinking"]
        self.assertEqual(rb(every), 4)
        del every["reasoning"]["max_tokens"]
        self.assertEqual(rb(every), 1024)                           # then the effort ("low")
        self.assertEqual(rb({"reasoning_effort": "high", "thinking_budget": 77}), 77)    # an alias beats an effort
        self.assertIsNone(rb({"reasoning_effort": "low", "thinking_budget": 0}))         # ... also when it is 0
        self.assertEqual(rb({"reasoning_budget_tokens": None, "thinking_budget": None, "reasoning": {"max_tokens": 5}}), 5)
        self.svc.reasoning_budget_tokens = 20                       # the config default is last
        self.assertEqual(rb({}), 20)
        self.assertEqual(rb({"reasoning_effort": "medium"}), 4096)
        self.assertEqual(rb({"thinking_budget": 8}), 8)
        self.assertIsNone(rb({"thinking_budget": 0}))

    def test_each_alias_wraps_up_the_thinking_on_both_endpoints(self):
        wrap = REASONING_WRAP_UP.split("</think>")[0]
        for name, make in self.ALIASES.items():
            with self.subTest(api="openai", alias=name):
                self.engine.prompts.clear()
                code, b = self.openai(**make(20))
                self.assertEqual(code, 200, b)
                m = b["choices"][0]["message"]
                self.assertEqual((m["reasoning_content"], m["content"]),
                                 (ThinkingEngine.THOUGHT[:20] + wrap, ThinkingEngine.ANSWER))
                self.assertEqual(len(self.engine.prompts), 2)
            with self.subTest(api="anthropic", alias=name):
                self.engine.prompts.clear()
                code, b = self.anthropic(**make(20))
                self.assertEqual(code, 200, b)
                by = {x["type"]: x for x in b["content"]}
                self.assertEqual((by["thinking"]["thinking"], by["text"]["text"]),
                                 (ThinkingEngine.THOUGHT[:20] + wrap, ThinkingEngine.ANSWER))
                self.assertEqual(len(self.engine.prompts), 2)
        # max_tokens stays the overall cap: 20 thought + the wrap-up does not fit in 40, so it ends as without a budget
        code, b = self.openai(thinking_budget=20, max_tokens=40)
        self.assertEqual(b["choices"][0]["finish_reason"], "length")
        self.assertEqual(b["choices"][0]["message"]["reasoning_content"], ThinkingEngine.THOUGHT[:20])

    def test_precedence_end_to_end(self):
        code, b = self.openai(reasoning_budget_tokens=10, thinking_budget=30, reasoning_effort="low")
        wrap = REASONING_WRAP_UP.split("</think>")[0]
        self.assertEqual(b["choices"][0]["message"]["reasoning_content"], ThinkingEngine.THOUGHT[:10] + wrap)
        code, b = self.openai(thinking_budget=15, reasoning={"max_tokens": 30})
        self.assertEqual(b["choices"][0]["message"]["reasoning_content"], ThinkingEngine.THOUGHT[:15] + wrap)

    def test_a_bad_value_is_a_400_naming_the_field(self):
        for name, make in self.ALIASES.items():
            for bad in ("lots", 2.5, True, [1], {"a": 1}):
                for api in (self.openai, self.anthropic):
                    with self.subTest(alias=name, value=bad, api=api.__name__):
                        self.engine.prompts.clear()
                        code, b = api(**make(bad))
                        self.assertEqual((code, b["error"]["type"]), (400, "invalid_request_error"))
                        self.assertIn(name, b["error"]["message"])
                        self.assertEqual(self.engine.prompts, [])
        code, b = self.openai(thinking_budget=5, reasoning_budget_tokens=10)     # the winner is the one checked
        self.assertEqual(code, 200, b)
        code, b = self.openai(thinking_budget="x", reasoning_budget_tokens=10)   # a loser is not looked at
        self.assertEqual(code, 200, b)

    def test_enable_thinking_false_is_the_templates_no_thinking(self):
        def prompt(call, **extra):
            self.engine.prompts.clear()
            code, _ = call(**extra)
            self.assertEqual(code, 200)
            return list(self.engine.prompts[0])
        default = prompt(self.openai)
        off = prompt(self.openai, chat_template_kwargs={"enable_thinking": False})
        self.assertNotEqual(off, default)
        self.assertEqual(prompt(self.openai, enable_thinking=False), off)
        self.assertEqual(prompt(self.openai, enable_thinking=False, reasoning_effort="high"), off)
        self.assertEqual(prompt(self.openai, enable_thinking=True), default)
        self.assertEqual(prompt(self.openai, enable_thinking=None), default)
        a_default = prompt(self.anthropic)
        a_off = prompt(self.anthropic, thinking={"type": "disabled"})
        self.assertNotEqual(a_off, a_default)
        self.assertEqual(prompt(self.anthropic, enable_thinking=False), a_off)
        self.assertEqual(prompt(self.anthropic, enable_thinking=False, output_config={"effort": "high"}), a_off)
        self.assertEqual(prompt(self.anthropic, enable_thinking=True), a_default)
        # no thinking, so no budget to wrap up
        self.engine.THOUGHT = ""
        self.engine.prompts.clear()
        code, b = self.openai(enable_thinking=False, thinking_budget=5)
        self.assertEqual((code, len(self.engine.prompts)), (200, 1))
        for call in (self.openai, self.anthropic):
            code, b = call(enable_thinking="no")
            self.assertEqual(code, 400)
            self.assertIn("enable_thinking", b["error"]["message"])
        s, b = self.post("/v1/messages/count_tokens", {"model": "m", "messages": [{"role": "user", "content": "hi"}],
                                                       "enable_thinking": False})
        s0, b0 = self.post("/v1/messages/count_tokens", {"model": "m", "messages": [{"role": "user", "content": "hi"}]})
        s1, b1 = self.post("/v1/messages/count_tokens", {"model": "m", "messages": [{"role": "user", "content": "hi"}],
                                                         "thinking": {"type": "disabled"}})
        self.assertEqual(b["input_tokens"], b1["input_tokens"])
        self.assertNotEqual(b["input_tokens"], b0["input_tokens"])

    def test_an_alias_asks_for_thinking_when_anthropic_thinking_is_on_request(self):
        msgs = {"messages": [{"role": "user", "content": "hi"}]}
        self.assertEqual(anthropic_to_messages(msgs, False)[2], {"enable_thinking": False})
        for extra in ({"reasoning_budget_tokens": 30}, {"thinking_budget": 30}, {"reasoning": {"max_tokens": 30}}):
            with self.subTest(extra=extra):
                self.assertNotIn("enable_thinking", anthropic_to_messages({**msgs, **extra}, False)[2])
        self.assertEqual(openai_to_messages({**msgs, "enable_thinking": False})[2], {"enable_thinking": False})


if __name__ == "__main__":
    unittest.main()
