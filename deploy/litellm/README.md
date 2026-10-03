# Claude Desktop gateway

This gateway maps `claude-orca` to Strata's Anthropic Messages endpoint. Both
sides use `/v1/messages`; there is no OpenAI conversion. Configure the model ID,
context limits and endpoint for your deployment in `config.yaml`, copied from
`config.example.yaml`. Export `STRATA_API_KEY` and pass it into the container.

Run `docker compose up -d --build` on Linux. The proxy listens on loopback port
4000. For private access from another Tailscale device, use a free HTTPS port:

```sh
sudo tailscale serve --bg --https=9443 http://127.0.0.1:4000
```

Use the URL printed by Serve as Claude Desktop's Gateway base URL. The model ID
is `claude-orca`. Leave the 1M context option off. Gateway mode uses separate local
conversation storage; it does not show the existing claude.ai conversation list.

The pinned LiteLLM image omits capability metadata from its model listings.
`patch_models.py` includes configured `capabilities` in both listing formats.
The compatibility flags in the config also prevent LiteLLM from dropping native
`thinking.type=adaptive` and `output_config.effort` for an unknown model.
Review both patch anchors before upgrading the image.

Strata maps effort to a thinking cap: minimal 256 (OpenAI only), low 512, medium
1024, high 2048, xhigh 4096, max unlimited. An explicit
`reasoning_budget_tokens` overrides effort; the total `max_tokens` still applies.
Effort is advertised under `capabilities.effort` in model list/detail responses.

Validation: syntax and mock-engine mapping tests; a Messages request through
LiteLLM with low effort reached 512 thinking tokens, then continued with text.
This verifies routing and the cap, not answer quality or every level end to end.
