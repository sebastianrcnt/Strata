"""serve/test_family_name.py - the API's model name per model family (NEW-MODELS-PLAN.md T0): the config's
"model_name" wins; without one, DeepSeek-V4.1's name for "arch": "deepseek41", and Flash-Next's for everything else.

Ported from upstream Strata 0.2.8, adapted: upstream also names a Qwen config without "model_name" by its arch
(qwen35moe -> qwen3.6-35b-a3b); this fork keeps the name such a config always had (qwen3.8-flash-next), so only the
DeepSeek family gets a name of its own.

    python -m unittest serve.test_family_name
"""
from __future__ import annotations

import unittest

from serve.server import FAMILY_MODEL_NAMES, default_model_name


class FamilyName(unittest.TestCase):
    def test_config_name_wins(self):
        self.assertEqual(default_model_name({"model_name": "qwen3.8-flash-next-q2_0", "arch": "qwen35moe"}),
                         "qwen3.8-flash-next-q2_0")

    def test_per_family(self):
        self.assertEqual(default_model_name({"arch": "qwen35moe"}), "qwen3.8-flash-next")     # (fork: unchanged)
        self.assertEqual(default_model_name({"arch": "qwen4exp"}), "qwen3.8-flash-next")
        self.assertEqual(default_model_name({"arch": "deepseek41"}), "deepseek-v4.1-flash")

    def test_old_configs(self):
        self.assertEqual(default_model_name({}), "qwen3.8-flash-next")
        self.assertEqual(default_model_name({"arch": "something-else"}), "qwen3.8-flash-next")
        self.assertEqual(set(FAMILY_MODEL_NAMES), {"qwen4exp", "qwen35moe", "deepseek41"})


if __name__ == "__main__":
    unittest.main()
