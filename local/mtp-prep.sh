#!/usr/bin/env bash
cd /home/coolguy/ml/strata
export PYTHONPATH=/home/coolguy/.claude/jobs/dc84a27e/tmp/dnsretry
export STRATA_GGUF_PY=$PWD/third_party/llama.cpp/gguf-py
for i in $(seq 1 30); do .venv/bin/python tools/mtp_fetch.py fetch --out mtp && break; echo "attempt $i failed"; sleep 5; done
.venv/bin/python tools/mtp_pack.py --src mtp --experts q2_0 --out mtp/mtp-q2_0.gguf &&
.venv/bin/python tools/mtp_rt.py --gguf mtp/mtp-q2_0.gguf --out mtp/rt &&
cp data/draft_vocab.bin mtp/rt/draft_vocab.bin
echo rc=$?
