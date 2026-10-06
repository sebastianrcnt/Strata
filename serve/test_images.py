"""serve/test_images.py - images are taken only inline (data: URLs, Anthropic base64 blocks): a request naming an
http(s) URL or a file path gets a 400 and nothing is fetched or read.  Mock engine, fake encoder (no GPU).

    python -m unittest serve.test_images -v
"""
from __future__ import annotations

import base64
import json
import re
import socket
import sys
import tempfile
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from serve.frontend import IMAGES_INLINE, ChatTemplate, anthropic_to_messages, openai_to_messages  # noqa: E402
from serve.server import ByteTokenizer, MockEngine, Service, Vision, serve  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
PNG = b"\x89PNG\r\n\x1a\nfake-test-image"
B64 = base64.b64encode(PNG).decode()
DATA_URL = "data:image/png;base64," + B64


class FakeVision:
    """Takes each source through Vision.load (the real one) and records the bytes; 3 rows per image."""

    def __init__(self, d):
        self.dir = Path(d)
        self.rows = self.dir / "img.sve"
        self.rows.write_bytes(b"rows")
        self.loaded = []

    def alive(self):
        return True

    def encode(self, source):
        self.loaded.append(Vision.load(source))
        return self.rows, 3


class InlineImagesOnly(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        tok = ByteTokenizer()
        cls.vision = FakeVision(cls.tmp.name)
        cls.svc = Service(MockEngine(tok, "</think>\n\nok", max_context=4096), tok,
                          ChatTemplate(ROOT / "serve/chat_template.jinja"), vision=cls.vision)
        cls.httpd = serve(cls.svc, port=0)
        cls.base = f"http://127.0.0.1:{cls.httpd.server_address[1]}"
        cls.picture = Path(cls.tmp.name) / "picture.png"
        cls.picture.write_bytes(PNG)

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()
        cls.tmp.cleanup()

    def setUp(self):
        self.vision.loaded.clear()

    def post(self, path, body):
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(),
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            with e:
                return e.code, json.loads(e.read())

    def openai(self, part):
        return self.post("/v1/chat/completions", {"model": "x", "max_tokens": 5, "messages": [
            {"role": "user", "content": [{"type": "text", "text": "what is this?"}, part]}]})

    def anthropic(self, source):
        return self.post("/v1/messages", {"model": "x", "max_tokens": 5, "messages": [
            {"role": "user", "content": [{"type": "text", "text": "what is this?"},
                                         {"type": "image", "source": source}]}]})

    def assert_refused(self, status, body):
        self.assertEqual(status, 400, body)
        self.assertIn("images must be sent inline as data: URLs", body["error"]["message"])
        self.assertEqual(self.vision.loaded, [])

    def test_data_url_images_still_work(self):
        for status, body in (self.openai({"type": "image_url", "image_url": {"url": DATA_URL}}),
                             self.openai({"type": "image_url", "image_url": DATA_URL}),
                             self.openai({"type": "input_image", "image_url": DATA_URL}),
                             self.anthropic({"type": "base64", "media_type": "image/png", "data": B64})):
            self.assertEqual(status, 200, body)
        self.assertEqual(self.vision.loaded, [PNG] * 4)

    def test_url_images_are_refused_not_fetched(self):
        # a listener the server would reach if it fetched the URL: it must see no connection
        trap = socket.socket()
        trap.bind(("127.0.0.1", 0))
        trap.listen(4)
        trap.settimeout(0.5)
        self.addCleanup(trap.close)
        local = f"http://127.0.0.1:{trap.getsockname()[1]}/x.png"
        for url in (local, "https://example.com/x.png"):
            with self.subTest(url=url):
                self.assert_refused(*self.openai({"type": "image_url", "image_url": {"url": url}}))
                self.assert_refused(*self.openai({"type": "input_image", "image_url": url}))
                self.assert_refused(*self.anthropic({"type": "url", "url": url}))
        with self.assertRaises(socket.timeout):
            trap.accept()

    def test_path_images_are_refused_not_read(self):
        for path in (str(self.picture), "file://" + str(self.picture)):
            with self.subTest(path=path):
                self.assert_refused(*self.openai({"type": "image_url", "image_url": {"url": path}}))
                self.assert_refused(*self.anthropic({"type": "path", "path": path}))
                self.assert_refused(*self.anthropic({"path": path}))
                self.assert_refused(*self.anthropic({"url": path}))

    def test_missing_source_is_refused(self):
        self.assert_refused(*self.openai({"type": "image_url"}))
        self.assert_refused(*self.anthropic({}))

    def test_converters_refuse_before_anything_runs(self):
        with self.assertRaisesRegex(ValueError, re.escape(IMAGES_INLINE)):
            openai_to_messages({"messages": [{"role": "user", "content": [
                {"type": "image_url", "image_url": {"url": "https://example.com/x.png"}}]}]})
        with self.assertRaisesRegex(ValueError, re.escape(IMAGES_INLINE)):
            anthropic_to_messages({"messages": [{"role": "user", "content": [
                {"type": "image", "source": {"type": "url", "url": "https://example.com/x.png"}}]}]})

    def test_the_encoder_s_loader_takes_only_data_urls(self):
        self.assertEqual(Vision.load(DATA_URL), PNG)
        for source in ("https://example.com/x.png", str(self.picture), "file://" + str(self.picture), "", None):
            with self.subTest(source=source), self.assertRaisesRegex(ValueError, re.escape(IMAGES_INLINE)):
                Vision.load(source)


if __name__ == "__main__":
    unittest.main()
