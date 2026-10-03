"""GPU handoff lifecycle tests without starting an encoder, engine or CUDA context."""
import io
import queue
import tempfile
import threading
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

from serve.server import EngineDied, StrataEngine, Vision, Service


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

    def _start(self):
        self.events.append(("start",))
        self.stopped = False
        if self.failure == "start":
            raise ValueError("encoder startup failed")

    def unload(self):
        self.events.append(("unload",))
        self.stopped = True

    def _encode_data(self, key, data):
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

    def test_invalid_swap_config_is_rejected_without_spawning(self):
        for config in ({"gpu": False}, {"gpu": True, "headroom_mib": 0},
                       {"gpu": True, "headroom_mib": True}):
            with mock.patch("serve.server.subprocess.Popen") as spawn:
                with self.assertRaises(ValueError):
                    Vision({"exe": "unused", "mmproj": "unused", "model": "unused",
                            "expert_swap": True, **config})
                spawn.assert_not_called()


if __name__ == "__main__":
    unittest.main()
