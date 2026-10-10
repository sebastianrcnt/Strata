"""serve/test_logit_bias.py - OpenAI logit_bias: the checks (a 400 for anything off), the engine line's `bias=` key
(and no key, the line of before, without one), and /v1/messages leaving it alone; against the mock engine.

    python -m unittest serve.test_logit_bias -v
"""
from __future__ import annotations

import json
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from serve.frontend import LOGIT_BIAS_KEY, MAX_LOGIT_BIAS, ChatTemplate, parse_logit_bias  # noqa: E402
from serve.server import ByteTokenizer, MockEngine, Service, StrataEngine, serve  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
VOCAB = 1000


class Parse(unittest.TestCase):
    def test_absent_null_empty(self):
        for req in ({}, {"logit_bias": None}, {"logit_bias": {}}):
            self.assertIsNone(parse_logit_bias(req, VOCAB))

    def test_valid(self):
        got = parse_logit_bias({"logit_bias": {"5": -100, " 17 ": 2.5, "999": 100, "0": 0}}, VOCAB)
        self.assertEqual(got, {5: -100.0, 17: 2.5, 999: 100.0, 0: 0.0})
        self.assertTrue(all(isinstance(v, float) for v in got.values()))

    def test_exactly_300(self):
        self.assertEqual(len(parse_logit_bias({"logit_bias": {str(i): 1 for i in range(MAX_LOGIT_BIAS)}}, VOCAB)), 300)

    def test_bad(self):
        bad = {"not an object": [[5, 1]], "a string": "5:1", "too many": {str(i): 1 for i in range(301)},
               "negative id": {"-1": 1}, "past the vocabulary": {str(VOCAB): 1}, "not a number key": {"abc": 1},
               "float key": {"1.5": 1}, "empty key": {"": 1}, "unicode digits": {"٥": 1},
               "too big": {"5": 100.01}, "too small": {"5": -101}, "a string value": {"5": "1"},
               "a bool value": {"5": True}, "null value": {"5": None}, "nan": {"5": float("nan")},
               "inf": {"5": float("inf")}, "repeated id": {"5": 1, "05": 2}}
        for name, value in bad.items():
            with self.subTest(name), self.assertRaises(ValueError):
                parse_logit_bias({"logit_bias": value}, VOCAB)

    def test_message_says_whose_ids(self):
        with self.assertRaises(ValueError) as cm:
            parse_logit_bias({"logit_bias": {"5000": 1}}, VOCAB)
        self.assertIn("not OpenAI's", str(cm.exception))


class EngineLine(unittest.TestCase):
    BASE = {"temperature": 0.7, "top_p": 0.9, "top_k": 20, "seed": 3}

    def test_no_bias_is_the_line_of_before(self):
        before = StrataEngine.sampling_keys(self.BASE)
        for extra in ({}, {LOGIT_BIAS_KEY: None}, {LOGIT_BIAS_KEY: {}}, {"logit_bias": {"5": -100}}):
            self.assertEqual(StrataEngine.sampling_keys({**self.BASE, **extra}), before)
        self.assertNotIn("bias=", before)

    def test_bias_key(self):
        keys = StrataEngine.sampling_keys({**self.BASE, LOGIT_BIAS_KEY: {17: 2.5, 5: -100.0, 9: 1e-05}})
        self.assertIn(" bias=5:-100.0,9:1e-05,17:2.5", keys)
        self.assertNotIn(" ", keys.split("bias=")[1].split(" cvec")[0])   # one token on the line: no spaces

    def test_junk_never_reaches_the_engine(self):
        self.assertEqual(StrataEngine.bias_key({LOGIT_BIAS_KEY: {"5": 1, -1: 2, 7: 101, 8: True, True: 1}}), "")
        self.assertEqual(StrataEngine.bias_key({LOGIT_BIAS_KEY: {i: 1.0 for i in range(301)}}), "")
        self.assertEqual(StrataEngine.bias_key({LOGIT_BIAS_KEY: {4: 1.0, -3: 2.0}}), " bias=4:1.0")


class SamplingRecorder(MockEngine):
    def generate(self, ids, max_new, sampling, cancel, embeddings=None):
        self.sampling = dict(sampling or {})
        yield from super().generate(ids, max_new, sampling, cancel, embeddings)


class Http(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tok = ByteTokenizer()
        cls.engine = SamplingRecorder(tok, "</think>\n\nok", max_context=4096)
        cls.svc = Service(cls.engine, tok, ChatTemplate(ROOT / "serve/chat_template.jinja"))
        cls.httpd = serve(cls.svc, port=0)
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def post(self, path, body):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def chat(self, **extra):
        self.engine.sampling = None
        return self.post("/v1/chat/completions", {"model": "x", "messages": [{"role": "user", "content": "hi"}],
                                                  "max_tokens": 16, **extra})

    def test_vocab_size(self):
        self.assertEqual(self.svc.vocab_size(), 256 + len(ByteTokenizer.SPECIALS))

    def test_openai_bias_reaches_the_engine(self):
        for stream in (False, True):
            with self.subTest(stream=stream):
                status, _ = self.chat(logit_bias={"104": -100, "33": 5}, stream=stream)
                self.assertEqual(status, 200)
                self.assertEqual(self.engine.sampling[LOGIT_BIAS_KEY], {104: -100.0, 33: 5.0})
                self.assertIn(" bias=33:5.0,104:-100.0", StrataEngine.sampling_keys(self.engine.sampling))

    def test_openai_without_bias(self):
        status, _ = self.chat()
        self.assertEqual(status, 200)
        self.assertIsNone(self.engine.sampling.get(LOGIT_BIAS_KEY))
        self.assertNotIn("bias=", StrataEngine.sampling_keys(self.engine.sampling))

    def test_smuggled_private_key_is_replaced(self):
        status, _ = self.chat(**{LOGIT_BIAS_KEY: {"7": 100}})
        self.assertEqual(status, 200)
        self.assertIsNone(self.engine.sampling.get(LOGIT_BIAS_KEY))

    def test_bad_bias_is_a_400_before_the_engine(self):
        for bad in ({"999999": 1}, {"5": 200}, {"x": 1}, [1, 2], {str(i): 1 for i in range(301)}):
            with self.subTest(bad=bad):
                status, raw = self.chat(logit_bias=bad)
                self.assertEqual(status, 400)
                err = json.loads(raw)["error"]
                self.assertEqual(err["type"], "invalid_request_error")
                self.assertIn("logit_bias", err["message"])
                self.assertIsNone(self.engine.sampling)          # the engine never ran

    def test_anthropic_ignores_it(self):
        for field in ("logit_bias", LOGIT_BIAS_KEY):
            with self.subTest(field=field):
                self.engine.sampling = None
                status, _ = self.post("/v1/messages", {"model": "x", "max_tokens": 16, field: {"104": -100},
                                                       "messages": [{"role": "user", "content": "hi"}]})
                self.assertEqual(status, 200)
                self.assertIsNone(self.engine.sampling.get(LOGIT_BIAS_KEY))
                self.assertNotIn("bias=", StrataEngine.sampling_keys(self.engine.sampling))


if __name__ == "__main__":
    unittest.main()
