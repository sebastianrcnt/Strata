// The engine's expert usage (GET /experts, from its USAGE lines): per (layer, expert), the routed lookups the VRAM
// cache served, those computed on a GPU from outside it (PCIe or another card) and those the CPU computed, since the
// engine started, and which experts the VRAM cache holds now. `before` is the report before the last request, so
// now - before is that request alone.

function bytes(b64) {
  return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
}

// one report -> {vram, gpu, cpu: Uint32Array(n), held: Uint8Array(n)}
export function decodeUsage(b64, layers, experts) {
  const n = layers * experts, b = bytes(b64);
  if (b.length < n * 12 + Math.ceil(n / 8)) return null;
  const u = new Uint32Array(b.buffer, 0, n * 3);
  const held = new Uint8Array(n);
  for (let i = 0; i < n; i++) held[i] = (b[n * 12 + (i >> 3)] >> (i & 7)) & 1;
  return {vram: u.slice(0, n), gpu: u.slice(n, 2 * n), cpu: u.slice(2 * n), held};
}

// the counts of `now` minus those of `before` (the last request), residency from `now`
export function since(now, before) {
  if (!before) return now;
  const d = (a, b) => a.map((v, i) => Math.max(0, v - b[i]));
  return {vram: d(now.vram, before.vram), gpu: d(now.gpu, before.gpu), cpu: d(now.cpu, before.cpu), held: now.held};
}

// the numbers the Memory tab shows, from one set of counts
export function summarize(u, layers, experts) {
  const n = layers * experts;
  let vram = 0, gpu = 0, cpu = 0, held = 0, used = 0;
  const total = new Float64Array(n);
  const perLayer = Array.from({length: layers}, () => ({vram: 0, all: 0, held: 0}));
  for (let i = 0; i < n; i++) {
    const t = u.vram[i] + u.gpu[i] + u.cpu[i];
    total[i] = t;
    vram += u.vram[i]; gpu += u.gpu[i]; cpu += u.cpu[i]; held += u.held[i];
    if (t > 0) used++;
    const L = perLayer[(i / experts) | 0];
    L.vram += u.vram[i]; L.all += t; L.held += u.held[i];
  }
  const all = vram + gpu + cpu;
  // how few experts take 90% of the lookups (the busiest first)
  const sorted = Array.from(total).sort((a, b) => b - a);
  let acc = 0, top90 = 0;
  while (top90 < n && acc < .9 * all) acc += sorted[top90++];
  // the busiest experts the cache does not hold: what it misses most
  const missed = [];
  for (let i = 0; i < n; i++) if (!u.held[i] && total[i] > 0) missed.push(i);
  missed.sort((a, b) => total[b] - total[a]);
  return {
    all, vram, gpu, cpu, held, used, n, top90, total,
    hit: all ? vram / all : null,
    perLayer: perLayer.map((L) => ({...L, hit: L.all ? L.vram / L.all : null})),
    missed: missed.slice(0, 12).map((i) => ({layer: (i / experts) | 0, expert: i % experts, lookups: total[i],
                                             cpu: total[i] ? u.cpu[i] / total[i] : 0})),
  };
}
