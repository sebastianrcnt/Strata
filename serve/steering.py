"""Steering vectors made and switched while the model serves (the steering studio, tools/steer_studio.py).

The engine was started with one control vector (--control-vector-scaled, `cvec=1`); this module adds named ones:

    measure   both sides of a contrast set read through the engine's prompt path with the stock model
              (GEN sums=FILE), each layer's mean residual on each side, their difference: the "draft" vector
    draft     the draft loaded at a scale over layers first..last (CVEC_LOAD 2 ...), for A/B chats
    save      the draft kept under a name: requests use it as `"strata_steer": NAME` or `"model": "steer-NAME"`

A vector is a raw file of n_layers x n_embd float32 (row l: layer l's output; row 0 is zero), the same numbers
tools/steer_build.py writes as `direction.<l>` - but each layer's row set to the length of that layer's mean
residual, so a scale is the share of the residual the vector pushes by (0.025 = 2.5%; orca-yok's 0.22 is about that).  Saved vectors live in `<dir>/<name>.f32` with `<name>.json` (scale,
layers, the pairs), and are loaded into every engine start.  The admin calls need the token in `<dir>/admin.token`
(header X-Steer-Token); using a saved vector does not.
"""
from __future__ import annotations

import array
import hmac
import json
import math
import os
import re
import secrets
import struct
import threading
import time
from pathlib import Path

STEER_ID_KEY = "_steer_id"       # the engine id of the request's vector (Service.run sets it, never the request)
STEER_SUMS_KEY = "_steer_sums"   # a measurement's sums file (Steering.measure only)
DRAFT, DRAFT_ID = "draft", 2
NAME = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")


def wrap_assistant(texts) -> str:
    """Each text as one assistant turn, as tools/steer_build.py feeds a contrast set (`\\n` = a line break)."""
    return "".join(f"<|im_start|>assistant\n{t.strip().replace(chr(92) + 'n', chr(10))}<|im_end|>\n" for t in texts)


class Steering:
    def __init__(self, svc, root: Path, n_layers: int = 48, n_embd: int = 2560, hc: int = 4,
                 cover=(1, 47)):
        self.svc, self.root = svc, Path(root)
        self.L, self.N, self.hc = n_layers, n_embd, hc
        self.cover = (int(cover[0]), int(cover[1]))   # the layers the engine may steer (--cvec-cover)
        self.root.mkdir(parents=True, exist_ok=True)
        tok = self.root / "admin.token"
        if not tok.exists():
            tok.write_text(secrets.token_urlsafe(24) + "\n")
            os.chmod(tok, 0o600)
        self.token = tok.read_text().strip()
        self.lock = threading.Lock()
        self.ids: dict[str, int] = {}                 # name -> engine id, of the vectors loaded into the engine
        self.draft: dict | None = None                # the draft's meta (scale, first, last, pairs, norms)
        meta = self.root / "_draft.json"
        if meta.exists() and (self.root / "_draft.f32").exists():
            try:
                self.draft = json.loads(meta.read_text())
            except ValueError:
                self.draft = None

    # ---- auth and lookup
    def authorized(self, headers) -> bool:
        given = headers.get("X-Steer-Token", "")
        return bool(given) and hmac.compare_digest(given.encode(), self.token.encode())

    def saved(self) -> list[dict]:
        out = []
        for m in sorted(self.root.glob("*.json")):
            if m.name.startswith("_"):
                continue
            try:
                d = json.loads(m.read_text())
            except ValueError:
                continue
            if (self.root / f"{m.stem}.f32").exists():
                out.append({**d, "name": m.stem})
        return out

    def models(self) -> list[str]:
        return [f"steer-{d['name']}" for d in self.saved()]

    def engine_id(self, sampling: dict) -> int | None:
        """The engine id of the vector a request names (`strata_steer`, or model `steer-NAME`); None for none."""
        name = sampling.get("strata_steer")
        model = sampling.get("model")
        if not name and isinstance(model, str) and model.startswith("steer-"):
            name = model[len("steer-"):]
        if not name:
            return None
        if not isinstance(name, str) or name not in self.ids:
            raise ValueError(f"strata_steer: no steering vector {name!r} (saved: "
                             f"{', '.join(n for n in self.ids if n != DRAFT) or 'none'})")
        return self.ids[name]

    # ---- the engine side (the caller holds svc.fifo)
    def _load(self, eid: int, path: Path, scale: float, first: int, last: int) -> int:
        line = self.svc.engine.command(f"CVEC_LOAD {eid} {float(scale)!r} {int(first)} {int(last)} {path}", "CVEC_OK")
        return int(line.split()[2])

    def _free_id(self) -> int:
        used = set(self.ids.values())
        for i in range(3, 256):
            if i not in used:
                return i
        raise ValueError("too many steering vectors (253): delete some")

    def replay(self) -> None:
        """Every vector into a freshly started engine (it starts with only its own)."""
        self.ids.clear()
        if not hasattr(self.svc.engine, "command"):
            return
        if str(self.svc.engine.info.get("cvec", 0)) in ("0", "", "None"):
            print("[strata] steering: the engine has no control vector loaded; the studio cannot steer", flush=True)
            return
        if self.draft is not None:
            try:
                self._load(DRAFT_ID, self.root / "_draft.f32", self.draft["scale"], self.draft["first"],
                           self.draft["last"])
                self.ids[DRAFT] = DRAFT_ID
            except (ValueError, KeyError) as e:
                print(f"[strata] steering: the draft did not load: {e}", flush=True)
        for d in self.saved():
            try:
                eid = self._free_id()
                self._load(eid, self.root / f"{d['name']}.f32", d["scale"], d["first"], d["last"])
                self.ids[d["name"]] = eid
            except (ValueError, KeyError) as e:
                print(f"[strata] steering: {d.get('name')} did not load: {e}", flush=True)
        if self.ids:
            print(f"[strata] steering vectors: {', '.join(self.ids)}", flush=True)

    def _sums(self, path: Path) -> tuple[array.array, int]:
        raw = path.read_bytes()
        tokens = struct.unpack_from("<q", raw)[0]
        a = array.array("d")
        a.frombytes(raw[8:8 + self.L * self.N * 8])
        if len(a) != self.L * self.N or tokens <= 0:
            raise ValueError("the engine wrote no residual sums (is the prompt path on?)")
        return a, tokens

    def _capture(self, text: str, path: Path) -> tuple[array.array, int]:
        ids = self.svc.tok.encode(text, parse_special=True)
        if len(ids) < 2:
            raise ValueError("a side of the contrast set is empty")
        if len(ids) + 1 > getattr(self.svc.engine, "max_context", 1 << 30):
            raise ValueError(f"a side is {len(ids)} tokens, more than the context holds")
        path.unlink(missing_ok=True)
        for _ in self.svc.engine.generate(ids, 1, {STEER_SUMS_KEY: str(path)}, threading.Event()):
            pass
        return self._sums(path)

    # ---- the studio's calls
    def measure(self, pos: list[str], neg: list[str], scale: float, first: int, last: int) -> dict:
        pos = [p for p in (x.strip() for x in pos) if p]
        neg = [n for n in (x.strip() for x in neg) if n]
        if not pos or not neg:
            raise ValueError("give at least one text on each side")
        t0 = time.time()
        with self.svc.fifo:
            self.svc.ensure_loaded()
            sp, tp = self._capture(wrap_assistant(pos), self.root / "_pos.sums")
            sn, tn = self._capture(wrap_assistant(neg), self.root / "_neg.sums")
            kp, kn = 1.0 / (tp * self.hc), 1.0 / (tn * self.hc)
            diff = array.array("f", (p * kp - n * kn for p, n in zip(sp, sn)))
            for j in range(self.N):                       # layer 0 is the embedding: never steered
                diff[j] = 0.0
            norms, ratios = [], []
            for l in range(self.L):
                o = l * self.N
                d = math.sqrt(sum(x * x for x in diff[o:o + self.N]))
                m = math.sqrt(sum(((p * kp + n * kn) / 2) ** 2 for p, n in zip(sp[o:o + self.N], sn[o:o + self.N])))
                norms.append(round(d, 4))
                ratios.append(round(d / m, 4) if m > 0 else 0.0)
                # each layer's direction to the length of that layer's mean residual: a scale is then the share of
                # the residual it pushes by (0.025 = 2.5%), whatever the number of pairs or their length
                k = m / d if d > 0 and m > 0 else 0.0
                for j in range(o, o + self.N):
                    diff[j] *= k
            (self.root / "_draft.f32").write_bytes(diff.tobytes())
            self.draft = {"scale": float(scale), "first": int(first), "last": int(last),
                          "pairs": {"pos": pos, "neg": neg}, "tokens": [tp, tn], "norms": norms, "ratios": ratios,
                          "created": time.strftime("%Y-%m-%d %H:%M:%S")}
            (self.root / "_draft.json").write_text(json.dumps(self.draft, ensure_ascii=False, indent=1))
            steered = self._load(DRAFT_ID, self.root / "_draft.f32", scale, first, last)
            self.ids[DRAFT] = DRAFT_ID
        return {**self.draft, "steered": steered, "seconds": round(time.time() - t0, 2)}

    def set_draft(self, scale: float, first: int, last: int) -> dict:
        if self.draft is None:
            raise ValueError("measure a contrast set first")
        with self.svc.fifo:
            self.svc.ensure_loaded()
            steered = self._load(DRAFT_ID, self.root / "_draft.f32", scale, first, last)
            self.draft.update(scale=float(scale), first=int(first), last=int(last))
            (self.root / "_draft.json").write_text(json.dumps(self.draft, ensure_ascii=False, indent=1))
            self.ids[DRAFT] = DRAFT_ID
        return {**self.draft, "steered": steered}

    def save(self, name: str, scale=None, first=None, last=None) -> dict:
        if not isinstance(name, str) or not NAME.match(name) or name == DRAFT:
            raise ValueError("name: 1-32 of a-z 0-9 _ - (not 'draft')")
        if self.draft is None:
            raise ValueError("measure a contrast set first")
        meta = {**self.draft}
        for k, v in (("scale", scale), ("first", first), ("last", last)):
            if v is not None:
                meta[k] = float(v) if k == "scale" else int(v)
        with self.svc.fifo:
            self.svc.ensure_loaded()
            (self.root / f"{name}.f32").write_bytes((self.root / "_draft.f32").read_bytes())
            (self.root / f"{name}.json").write_text(json.dumps(meta, ensure_ascii=False, indent=1))
            eid = self.ids.get(name) or self._free_id()
            steered = self._load(eid, self.root / f"{name}.f32", meta["scale"], meta["first"], meta["last"])
            self.ids[name] = eid
        return {"name": name, "model": f"steer-{name}", "steered": steered, **meta}

    def update(self, name: str, scale: float, first: int, last: int) -> dict:
        m = self.root / f"{name}.json"
        if not isinstance(name, str) or not NAME.match(name) or not m.exists():
            raise ValueError(f"no saved vector {name!r}")
        meta = json.loads(m.read_text())
        meta.update(scale=float(scale), first=int(first), last=int(last))
        with self.svc.fifo:
            self.svc.ensure_loaded()
            m.write_text(json.dumps(meta, ensure_ascii=False, indent=1))
            eid = self.ids.get(name) or self._free_id()
            steered = self._load(eid, self.root / f"{name}.f32", meta["scale"], meta["first"], meta["last"])
            self.ids[name] = eid
        return {"name": name, "steered": steered, **meta}

    def delete(self, name: str) -> dict:
        if not isinstance(name, str) or not NAME.match(name) or not (self.root / f"{name}.json").exists():
            raise ValueError(f"no saved vector {name!r}")
        with self.svc.fifo:
            eid = self.ids.pop(name, None)
            if eid is not None and self.svc.loaded():
                self.svc.engine.command(f"CVEC_DROP {eid}", "CVEC_OK")
            (self.root / f"{name}.json").unlink(missing_ok=True)
            (self.root / f"{name}.f32").unlink(missing_ok=True)
        return {"deleted": name}

    def status(self) -> dict:
        info = getattr(self.svc.engine, "info", {}) or {}
        return {"layers": self.L, "cover": list(self.cover), "engine_cvec": str(info.get("cvec", 0)),
                "loaded": dict(self.ids), "draft": self.draft, "saved": self.saved()}
