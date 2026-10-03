"""GPU handoff lifecycle tests without starting an encoder, engine or CUDA context."""
import io
import base64
import json
import socket
import sys
import time
import urllib.request
import urllib.error
import queue
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from serve.server import EngineStuck, EngineDied, StrataEngine, Vision, Service, enable_vision_swap, ByteTokenizer, MockEngine, serve
from serve.frontend import ChatTemplate


class HandoffEngine:
    def __init__(self, events, release_error=None, restore_error=None):
        self.events = events
        self.release_error, self.restore_error = release_error, restore_error

    def vision_memory(self, mib=None):
        self.events.append(("release", mib) if mib is not None else ("restore",))
        error = self.release_error if mib is not None else self.restore_error
        if error:
            raise error


class BatchVision(Vision):
    load = staticmethod(lambda source: source.encode())
    normalize = staticmethod(lambda data: data)

    def __init__(self, events, failure=None):
        self.events, self.failure = events, failure
        super().__init__({"exe": "never-spawn", "mmproj": "unused", "model": "unused",
                          "gpu": True, "expert_swap": True, "headroom_mib": 2048})

    def _start(self, cancel=None):
        self.events.append(("start",))
        self.stopped = False
        if self.failure == "start":
            raise ValueError("encoder startup failed")

    def unload(self):
        self.events.append(("unload",))
        self.stopped = True

    def _encode_data(self, key, data, cancel=None):
        if key not in self.cache:
            self.events.append(("encode", data.decode()))
            if self.failure == "encode":
                raise ValueError("invalid image")
            if self.failure == "cancel":
                raise KeyboardInterrupt()
            path = self.dir / (key + ".sve")
            path.write_bytes(data)
            self.cache[key] = (path, 1)
        return self.cache[key]


class VisionSwap(unittest.TestCase):
    def vision(self, events, failure=None):
        v = BatchVision(events, failure)
        self.addCleanup(lambda: __import__("shutil").rmtree(v.dir, ignore_errors=True))
        return v

    def test_no_resident_start_and_no_service_reload_for_dormant_encoder(self):
        events = []
        v = self.vision(events)
        self.assertEqual(events, [])
        self.assertTrue(v.stopped)
        self.assertFalse(Service._vision_down(SimpleNamespace(vision=v)))

    def test_batch_one_handoff_and_cache_hit_skips_handoff(self):
        events = []
        v = self.vision(events)
        e = HandoffEngine(events)
        first = v.encode_batch(["one", "two", "one"], e)
        self.assertEqual(events, [("release", 2048), ("start",), ("encode", "one"),
                                  ("encode", "two"), ("unload",), ("restore",)])
        self.assertEqual(first[0], first[2])
        events.clear()
        self.assertEqual(v.encode_batch(["two", "one"], e), [first[1], first[0]])
        self.assertEqual(events, [])

    def test_error_and_cancellation_restore_before_propagation(self):
        for failure, error in [("start", ValueError), ("encode", ValueError), ("cancel", KeyboardInterrupt)]:
            with self.subTest(failure=failure):
                events = []
                v = self.vision(events, failure)
                with self.assertRaises(error):
                    v.encode_batch(["one"], HandoffEngine(events))
                self.assertEqual(events[-2:], [("unload",), ("restore",)])
                self.assertTrue(v.stopped)

    def test_partial_release_failure_still_restores_without_start(self):
        events = []
        v = self.vision(events)
        with self.assertRaisesRegex(ValueError, "partial"):
            v.encode_batch(["one"], HandoffEngine(events, release_error=ValueError("partial release")))
        self.assertEqual(events, [("release", 2048), ("unload",), ("restore",)])

    def test_restore_failure_never_returns_embeddings(self):
        events = []
        v = self.vision(events)
        with self.assertRaises(EngineDied):
            v.encode_batch(["one"], HandoffEngine(events, restore_error=EngineDied("restore failed")))
        self.assertEqual(events[-2:], [("unload",), ("restore",)])

    def test_unreaped_encoder_leaves_engine_blocked_without_reallocating(self):
        events = []
        v = self.vision(events)
        e = HandoffEngine(events)
        def stuck():
            events.append(("unload",))
            raise TimeoutError("still alive")
        v.unload = stuck
        with self.assertRaises(EngineDied):
            v.encode_batch(["one"], e)
        self.assertTrue(e.vision_memory_failed)
        self.assertNotIn(("restore",), events)

    def engine(self, response):
        e = StrataEngine.__new__(StrataEngine)
        e.info = {"vision_lending": 1}
        e.proc = SimpleNamespace(stdin=io.StringIO())
        e.lines = queue.Queue()
        e.lines.put(response)
        return e

    def test_protocol_uses_real_newline_and_restore_clears_poison(self):
        e = self.engine("VISION_RELEASED ok\n")
        e.vision_memory(2048)
        self.assertEqual(e.proc.stdin.getvalue(), "VISION_RELEASE 2048\n")
        e.vision_memory_failed = True
        e.lines.put("VISION_RESTORED ok\n")
        e.vision_memory()
        self.assertFalse(e.vision_memory_failed)

    def test_failed_restore_poison_blocks_generation(self):
        e = self.engine("ERR cannot allocate chunk\n")
        with self.assertRaises(EngineDied):
            e.vision_memory()
        self.assertTrue(e.vision_memory_failed)
        with self.assertRaises(EngineDied):
            list(e.generate([1], 1, {}, threading.Event()))

    def test_legacy_engine_is_rejected_without_writing_a_command(self):
        e = self.engine("")
        e.info = {}
        with self.assertRaises(ValueError):
            e.vision_memory(2048)
        self.assertEqual(e.proc.stdin.getvalue(), "")

    def test_release_timeout_poison_is_not_reset_by_late_reply(self):
        e = self.engine("")
        e.lines = mock.Mock()
        e.lines.get.side_effect = queue.Empty()
        with self.assertRaises(EngineDied):
            e.vision_memory(2048)
        self.assertTrue(e.vision_memory_failed)
        self.assertTrue(e.vision_protocol_failed)
        written = e.proc.stdin.getvalue()
        e.lines.get.side_effect = None
        e.lines.get.return_value = "VISION_RESTORED ok\\n"
        with self.assertRaises(EngineDied):
            e.vision_memory()
        with self.assertRaises(EngineDied):
            e.vision_memory(2048)
        self.assertEqual(e.proc.stdin.getvalue(), written)
        with self.assertRaises(EngineDied):
            list(e.generate([1], 1, {}, threading.Event()))

    def test_explicit_release_error_does_not_poison_protocol(self):
        e = self.engine("ERR partial release\\n")
        with self.assertRaises(ValueError):
            e.vision_memory(2048)
        self.assertFalse(getattr(e, "vision_protocol_failed", False))
        e.lines.put("VISION_RESTORED ok\\n")
        e.vision_memory()
        self.assertFalse(e.vision_memory_failed)

    def test_swap_device_validation(self):
        cfg = {"args": ["--expert-cache", "auto"], "gpu": [0], "vision": {"expert_swap": True, "cuda_device": 0}}
        for device in (-1, True, 0.5, "0.5", "not-a-device", 8):
            with self.subTest(device=device):
                with self.assertRaises(ValueError):
                    enable_vision_swap({**cfg, "vision": {**cfg["vision"], "cuda_device": device}}, {})
        for overrides in ({"gpu": [0, 1]}, {"gpu": [-1]}, {"backend": "hip"}, {"gpu": None}):
            with self.subTest(overrides=overrides):
                with self.assertRaises(ValueError):
                    enable_vision_swap({**cfg, **overrides}, {})
        with self.assertRaises(ValueError):
            enable_vision_swap(cfg, {"CUDA_VISIBLE_DEVICES": "1"})
        env = {}
        self.assertTrue(enable_vision_swap({"args": ["--expert-cache", "auto"], "gpu": [2], "vision": {"expert_swap": True, "cuda_device": "2"}}, env))
        self.assertEqual(env["STRATA_EXPERT_VMM"], "1")
        self.assertTrue(enable_vision_swap({"args": ["--expert-cache", "auto"], "vision": {"expert_swap": True}}, {})) # same inherited visibility
        self.assertFalse(enable_vision_swap({}, {}))

    def test_remote_and_absent_expert_cache_rejected_at_startup(self):
        for args in ([], ["--expert-cache", "0"], ["--expert-cache", "auto", "--expert-cache-device1", "32"]):
            with self.assertRaises(ValueError):
                enable_vision_swap({"args": args, "vision": {"expert_swap": True}}, {})

    def test_unreaped_encoder_blocks_reload_and_preserves_process(self):
        tok = ByteTokenizer()
        e = MockEngine(tok, "ok")
        e.restart = mock.Mock()
        proc = mock.Mock()
        proc.poll.return_value = None
        v = Vision.__new__(Vision)
        v.proc = proc
        with self.assertRaises(EngineStuck):
            v._start()
        self.assertIs(v.proc, proc)
        v.expert_swap = True
        v.unload = mock.Mock(side_effect=__import__("subprocess").TimeoutExpired("encoder", 20))
        svc = Service(e, tok, ChatTemplate(Path(__file__).parent / "chat_template.jinja"), vision=v)
        with self.assertRaises(EngineStuck):
            svc.ensure_loaded()
        self.assertIs(v.proc, proc)
        e.restart.assert_not_called()
        self.assertTrue(e.vision_memory_failed)
        self.assertIn("explicitly unload", svc.engine_error(EngineDied("failed")))

    def test_poisoned_live_engine_requires_explicit_unload_not_new_image_retry(self):
        e = StrataEngine.__new__(StrataEngine)
        e.proc = SimpleNamespace(stdin=io.StringIO())
        e.info = {"vision_lending": 1}
        e.vision_memory_failed = True
        svc = Service(MockEngine(ByteTokenizer(), "ok"), ByteTokenizer(),
                      ChatTemplate(Path(__file__).parent / "chat_template.jinja"))
        svc.engine = e
        svc.loaded = lambda: True
        with self.assertRaises(EngineDied):
            svc.ensure_loaded()
        v = self.vision([])
        with self.assertRaises(EngineDied):
            v.encode_batch(["new"], e)
        self.assertEqual(e.proc.stdin.getvalue(), "")

    def test_invalid_swap_config_is_rejected_without_spawning(self):
        for config in ({"gpu": False}, {"gpu": True, "headroom_mib": 0},
                       {"gpu": True, "headroom_mib": True}):
            with mock.patch("serve.server.subprocess.Popen") as spawn:
                with self.assertRaises(ValueError):
                    Vision({"exe": "unused", "mmproj": "unused", "model": "unused",
                            "expert_swap": True, **config})
                spawn.assert_not_called()


class LiveChildEngine(MockEngine):
    def __init__(self, events):
        super().__init__(ByteTokenizer(), "ok", max_context=4096)
        self.events = events
        self.restored = threading.Event()
        self.released = threading.Event()

    def vision_memory(self, mib=None):
        self.events.append(("release", mib) if mib is not None else ("restore",))
        (self.released if mib is not None else self.restored).set()


class VisionHTTP(unittest.TestCase):
    """Real sockets and live subprocesses; no CUDA/model required."""
    def launch(self, mode):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        script = root / "encoder"
        marker = root / "encoding"
        if mode == "startup-hang":
            code = "time.sleep(60)"
        elif mode == "startup-error":
            code = 'print("ERR incompatible projector", flush=True)'
        else:
            code = 'print("READY 2560", flush=True)\nfor line in sys.stdin:\n if line.startswith("ENC "):\n  Path(%r).write_text("encoding")\n  time.sleep(60)\n elif line.startswith("QUIT"):\n  break' % str(marker)
        script.write_text("#!" + sys.executable + "\nimport sys,time\nfrom pathlib import Path\n" + code + "\n")
        script.chmod(0o755)
        v = Vision({"exe": str(script), "mmproj": "unused", "model": "unused", "gpu": True,
                    "expert_swap": True, "start_timeout_s": 0.3 if mode != "encode-hang" else 5,
                    "encode_timeout_s": 0.3 if mode == "encode-timeout" else 60})
        events = []
        e = LiveChildEngine(events)
        svc = Service(e, e.tok, ChatTemplate(Path(__file__).parent / "chat_template.jinja"), vision=v)
        httpd = serve(svc, port=0)
        self.addCleanup(httpd.server_close)
        self.addCleanup(httpd.shutdown)
        self.addCleanup(v.close)
        self.addCleanup(lambda: __import__("shutil").rmtree(v.dir, ignore_errors=True))
        return v, e, svc, httpd.server_address[1], marker

    def body(self, api):
        image = base64.b64encode(b"\x89PNG\r\n\x1a\nfake-test-image").decode()
        if api == "openai":
            return "/v1/chat/completions", {"model": "x", "max_tokens": 10,
                "messages": [{"role": "user", "content": [{"type": "image_url",
                              "image_url": {"url": "data:image/png;base64," + image}}]}]}
        return "/v1/messages", {"model": "x", "max_tokens": 10,
            "messages": [{"role": "user", "content": [{"type": "image", "source":
                          {"type": "base64", "media_type": "image/png", "data": image}}]}]}

    def test_http_disconnect_while_encoder_is_silent_restores_fifo(self):
        for api in ("openai", "anthropic"):
            with self.subTest(api=api):
                v, e, svc, port, marker = self.launch("encode-hang")
                path, body = self.body(api)
                data = json.dumps(body).encode()
                sock = socket.create_connection(("127.0.0.1", port))
                sock.sendall((f"POST {path} HTTP/1.0\r\nHost: localhost\r\nContent-Type: application/json\r\n"
                              f"Content-Length: {len(data)}\r\n\r\n").encode() + data)
                end = time.monotonic() + 3
                while not marker.exists() and time.monotonic() < end:
                    time.sleep(0.01)
                self.assertTrue(marker.exists(), "request must reach the live child's encode read before disconnect")
                sock.close()
                self.assertTrue(e.restored.wait(4), "client hang-up must reap encoder and restore promptly")
                self.assertIsNone(v.proc)
                self.assertTrue(svc.fifo.acquire(timeout=1))
                svc.fifo.release()
                self.assertEqual(e.events, [("release", 2048), ("restore",)])

    def test_startup_errors_and_timeouts_are_http_errors_with_restoration(self):
        for mode in ("startup-error", "startup-hang", "encode-timeout"):
            with self.subTest(mode=mode):
                v, e, _, port, _ = self.launch(mode)
                path, body = self.body("openai")
                req = urllib.request.Request(f"http://127.0.0.1:{port}" + path,
                    data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(req, timeout=5)
                self.assertEqual(error.exception.code, 503)
                response = json.load(error.exception)
                self.assertEqual(response["error"]["type"], "server_error")
                self.assertTrue(e.restored.wait(1))
                self.assertIsNone(v.proc)

    def test_persist_poison_guard_and_err_reply_are_immediate(self):
        e = StrataEngine.__new__(StrataEngine)
        e.proc = SimpleNamespace(stdin=io.StringIO())
        e.vision_memory_failed = True
        with self.assertRaises(EngineDied):
            e.persist()
        self.assertEqual(e.proc.stdin.getvalue(), "")
        e.vision_memory_failed = False
        e.lines = queue.Queue()
        e.lines.put("ERR expert memory is lent to vision\n")
        before = time.monotonic()
        with self.assertRaises(EngineDied):
            e.persist()
        self.assertLess(time.monotonic() - before, 0.1)


if __name__ == "__main__":
    unittest.main()
