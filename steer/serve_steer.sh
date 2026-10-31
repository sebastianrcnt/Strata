#!/bin/sh
# serve_steer.sh SCALE FIRST LAST [VECTOR]: (re)start the strata service with a steering vector loaded, off by default.
# Requests turn it on with "model": "orca-yok" (STEER_MODEL=name for another) or "experimental_speed_projection": true;
# STEER_DEFAULT=on switches it on for every request that does not say false.
# The steering studio (tools/steer_studio.py): this branchs engine and server are mounted over the images, with
# --cvec-cover STEER_COVER (default 1 47: the layers a studio vector may steer) and its API on /steer/*.
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
c["args"] += ["--control-vector-scaled", vec, "--control-vector-layer-range", first, last, "--cvec-mode", "add",
              "--cvec-cover", *os.environ.get("STEER_COVER", "1 47").split()]
cover = [int(x) for x in os.environ.get("STEER_COVER", "1 47").split()]
c["steering"] = {"dir": "/state/steer", "cover": [min(cover[0], int(first)), max(cover[1], int(last))]}
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
      - $S/../build/strata:/opt/strata/strata:ro
      - $S/../serve/server.py:/opt/strata/serve/server.py:ro
      - $S/../serve/steering.py:/opt/strata/serve/steering.py:ro
EOF
docker compose -f compose.yaml -f "$S/compose.steer.yaml" up -d --force-recreate strata
