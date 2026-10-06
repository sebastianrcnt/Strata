"""serve/server.py reads the engines CACHE line: RAM-parked conversations, and the disk tiers D/S fields
(conversation dir spill) without breaking on either form."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "serve"))
import server  # noqa: E402


def engine():
    e = server.StrataEngine.__new__(server.StrataEngine)   # no process: only the parser
    e.conv = {"budget_mib": 8192, "slots": 4, "reported": False, "live_tokens": 0, "bytes": 0, "evictions": 0,
              "parked": []}
    return e


class CacheLineTest(unittest.TestCase):
    def test_old_form(self):
        e = engine()
        e._parse_cache("CACHE 61859 5230401432 10 ae026a9e141f4a8f:15772:477002528\n")
        self.assertTrue(e.conv["reported"])
        self.assertEqual(e.conv["parked"], [{"key": "ae026a9e141f4a8f", "tokens": 15772, "bytes": 477002528}])
        self.assertEqual(e.conv["disk"], [])
        self.assertIsNone(e.conv["spill"])

    def test_disk_tier(self):
        e = engine()
        e._parse_cache("CACHE 0 0 3 ae026a9e141f4a8f:15772:477002528 D2f16b4f3a68d4469:168087:3396172401 "
                       "D122e0d7aefab79b5:185848:900 S7:3:1500000000\n")
        self.assertEqual(len(e.conv["parked"]), 1)
        self.assertEqual(e.conv["disk"], [{"key": "2f16b4f3a68d4469", "tokens": 168087, "bytes": 3396172401},
                                          {"key": "122e0d7aefab79b5", "tokens": 185848, "bytes": 900}])
        self.assertEqual(e.conv["spill"], {"spills": 7, "restores": 3, "pending_bytes": 1500000000})

    def test_garbage_keeps_the_last_report(self):
        e = engine()
        e._parse_cache("CACHE 1 2 3 D1:2\n")
        self.assertFalse(e.conv["reported"])


if __name__ == "__main__":
    unittest.main()
