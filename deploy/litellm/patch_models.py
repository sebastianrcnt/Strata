"""Preserve configured capability metadata in the pinned LiteLLM model endpoints."""
from pathlib import Path

root = Path('/app/.venv/lib/python3.13/site-packages/litellm')
p = root / 'proxy/utils.py'
s = p.read_text()
needle = '    if not include_metadata:\n        return base'
replacement = '''    # Local: preserve configured capability metadata for compatible backends.
    if llm_router is not None:
        for deployment in llm_router.model_list:
            if deployment.get("model_name") == lookup_model:
                capabilities = deployment.get("model_info", {}).get("capabilities")
                if isinstance(capabilities, dict):
                    base["capabilities"] = capabilities
                    break

'''
assert needle in s, 'LiteLLM listing implementation changed; review patch'
s = s.replace(needle, replacement + needle, 1)
compile(s, str(p), 'exec')
p.write_text(s)
p = root / 'llms/anthropic/common_utils.py'
s = p.read_text()
needle = '        "max_tokens": model.get("max_output_tokens"),'
assert needle in s, 'LiteLLM Anthropic listing implementation changed; review patch'
s = s.replace(needle, needle + '\n        **({"capabilities": model["capabilities"]} if model.get("capabilities") else {}),', 1)
compile(s, str(p), 'exec')
p.write_text(s)
