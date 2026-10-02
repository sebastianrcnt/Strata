#!/usr/bin/env bash
d=/home/coolguy/ml/qwen38/models/flash-next-uncensored-IQ3_XXS
url=https://huggingface.co/orcarouter/Qwen3.8-Flash-Next-Uncensored-GGUF/resolve/main/Qwen3.8-Flash-Next-Uncensored-IQ3_XXS-00001-of-00002.gguf
tok=$(cat /home/coolguy/.cache/huggingface/token)
for i in $(seq 1 60); do
  curl -sS -L -C - --speed-limit 1000000 --speed-time 30 -H "Authorization: Bearer $tok" -o $d/shard1.part "$url" && break
  echo "curl attempt $i failed; size $(stat -c %s $d/shard1.part)"; sleep 3
done
echo "size $(stat -c %s $d/shard1.part) expected 44637691008"
echo "$(sha256sum $d/shard1.part | cut -c1-64)" > $d/shard1.sha
if grep -q aaf57046943c6638480e8984835ce5ec29c486180ff71169d3c22c6929851b7b $d/shard1.sha; then
  mv $d/shard1.part $d/Qwen3.8-Flash-Next-Uncensored-IQ3_XXS-00001-of-00002.gguf && echo VERIFIED
else echo HASH_MISMATCH; cat $d/shard1.sha; fi
