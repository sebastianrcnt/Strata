#!/bin/sh
# Runs the deployed engine image once with the model mounted as the strata service mounts it, and steer/ at /work.
exec docker run --rm --gpus all --user 1000:1000 --ulimit memlock=-1 --entrypoint /opt/strata/strata \
  -v /home/coolguy/ml/qwen38/models/sc117-gsq-rco-abliterated-iq3xxs/IQ3_XXS:/models:ro \
  -v /home/coolguy/ml/strata/packs/sc117-gsq-rco-abliterated-iq3xxs:/assets/pack:ro \
  -v /home/coolguy/ml/strata/data:/assets/data:ro \
  -v /home/coolguy/ml/strata-steering/steer:/work -v /home/coolguy/ml/strata-steering/build/strata:/opt/strata/strata:ro -e STRATA_PREFILL_SUMS \
  strata-local:logitbias-3bab724 \
  --pack /assets/pack \
  --native /models/Qwen3.8-Flash-Next-GSQ-RCO-abliterated-IQ3_XXS-00001-of-00002.gguf \
  --ple-gguf /models/Qwen3.8-Flash-Next-GSQ-RCO-abliterated-IQ3_XXS-00002-of-00002.gguf \
  --expert-profile /assets/data/expert-profile.bin --expert-cache 6000 --max-context 4096 --kv int8 "$@"
