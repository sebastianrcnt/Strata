#!/bin/sh
# serve_steer.sh SCALE FIRST LAST [VECTOR]: (re)start the strata service with a steering vector loaded, off by default.
# Requests turn it on with "model": "orca-yok" (STEER_MODEL=name for another) or "experimental_speed_projection": true;
# STEER_DEFAULT=on switches it on for every request that does not say false.
set -e
D=/home/coolguy/ml/strata/deploy/docker
S=/home/coolguy/ml/strata-steering/steer
cd "$D"
if [ "$1" = off ]; then
  docker compose up -d --force-recreate strata
  exit
fi
VEC=${4:-profanity-ko.gguf}
python3 - "$D/config.json" "$S/config.steer.json" "/assets/steer/$VEC:$1" "$2" "$3" <<'EOF'
import json, os, sys
src, dst, vec, first, last = sys.argv[1:]
c = json.load(open(src))
c["args"] += ["--control-vector-scaled", vec, "--control-vector-layer-range", first, last, "--cvec-mode", "add"]
c.setdefault("sampling", {})["experimental_speed_projection"] = os.environ.get("STEER_DEFAULT") == "on"
c.setdefault("model_presets", {})[os.environ.get("STEER_MODEL", "orca-yok")] = {"experimental_speed_projection": True}
json.dump(c, open(dst, "w"), indent=2)
EOF
cat > "$S/compose.steer.yaml" <<EOF
services:
  strata:
    volumes:
      - $S/config.steer.json:/config/config.json:ro
      - $S:/assets/steer:ro
EOF
docker compose -f compose.yaml -f "$S/compose.steer.yaml" up -d --force-recreate strata
