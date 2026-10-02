"""Local effort policy: levels keep the same prompt; thinking ON/OFF remains functional."""
import unittest
from pathlib import Path
from serve.frontend import ChatTemplate

ROOT = Path(__file__).resolve().parents[1]

class NoEffortInjection(unittest.TestCase):
    def test_levels_do_not_add_instructions(self):
        messages = [{"role": "system", "content": "You are helpful."},
                    {"role": "user", "content": "Explain caching."}]
        for name in ("serve/chat_template.jinja", "local/chat_template-tail-effort.jinja",
                     "local/chat_template-orca-no-effort.jinja"):
            tpl = ChatTemplate(ROOT / name)
            for tools in (None, [{"type": "function", "function": {"name": "read", "parameters": {"type": "object"}}}]):
                with self.subTest(template=name, tools=bool(tools)):
                    base = tpl.render(messages, tools=tools, reasoning_effort="medium")
                    for effort in ("low", "high", "xhigh"):
                        self.assertEqual(base, tpl.render(messages, tools=tools, reasoning_effort=effort))
                    self.assertNotIn("Reasoning effort is set to", base)
                    self.assertNotEqual(base, tpl.render(messages, tools=tools, enable_thinking=False))
                    self.assertIn("You are helpful.", base)

if __name__ == "__main__":
    unittest.main()
