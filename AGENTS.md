# AGENTS.md

Strata runs the Qwen3.8-Flash-Next mixture-of-experts model (and its Coder, Swift 1.5 and Unsloth variants) on a
normal PC: one NVIDIA or AMD graphics card plus system RAM, on Windows or Linux. It has a C++/CUDA/HIP engine
(`src/`, `include/`), a Python server with an OpenAI- and Anthropic-compatible API and a web app (`serve/`), and a
one-click installer (`setup.py`, started by `START-HERE.bat` / `setup.sh`).

## Installing Strata for a user

Follow **[docs/AI_SETUP.md](docs/AI_SETUP.md)**: check the PC, pick the model by RAM, run setup non-interactively,
start and verify the server, and connect the user's apps. Never expose the server beyond `127.0.0.1` without
`--api-key`. As an alternative to shell commands, Strata's MCP server ([docs/MCP_SERVER.md](docs/MCP_SERVER.md))
offers the same steps as tools.

## Working on the code

- How the engine works, every measured number, the API and all settings: [docs/DETAILS.md](docs/DETAILS.md) and
  the [paper](docs/paper/Strata-Paper.pdf).
- AMD (HIP) build and validation: [docs/AMD_HIP.md](docs/AMD_HIP.md); multi-GPU: [docs/MULTI_GPU.md](docs/MULTI_GPU.md).
- Setup's own tests run without a GPU or downloads: `python tools/test_setup_<name>.py` (for example
  `tools/test_setup_amd.py`, `tools/test_setup_choices.py`).
- The web app's source is `serve/web-src` (Svelte, built with Bun + Vite into `serve/web`): after changing it, run
  `bun run build` there and commit the built files with it ([serve/web-src/README.md](serve/web-src/README.md)).
- Keep the docs' style: plain words, measured numbers with what they were measured on, no claims without a
  measurement.

## Fork branch workflow

- This fork is `sebastianrcnt/Strata`, remote `fork`; `origin` is upstream `Niko1221/Strata`.
- `main` is the fork integration branch and tracks `fork/main`.
- Start new features from the fork main on `feat/<feature-name>` branches. Do not implement features directly on `main` or continue the legacy `local-patches` branch.
- Validate feature changes before merging into the fork main. Push fork changes to `fork`, not upstream `origin`.
- Keep upstream integration separate from feature work and preserve local patches when resolving conflicts.
