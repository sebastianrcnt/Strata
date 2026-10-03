// The expert profile and the cache numbers for the Experts tab. data/expert-profile.bin ranks every (layer, expert)
// pair of the model, most used first; the engine fills its VRAM expert cache with the first `expert_slots` pairs
// when it starts (then swaps a few toward the conversation without reporting which).

// {layers, experts, ranked: base64 of uint16 (layer, expert) pairs} -> each pair's rank (pairs it omits come last)
export function decodeProfile({layers, experts, ranked}) {
  const bin = Uint8Array.from(atob(ranked), (c) => c.charCodeAt(0));
  const pairs = new Uint16Array(bin.buffer, 0, bin.length >> 1);
  const n = layers * experts;
  const rank = new Int32Array(n).fill(-1);
  let next = 0;
  for (let i = 0; i + 1 < pairs.length; i += 2) {
    const l = pairs[i], e = pairs[i + 1];
    if (l < layers && e < experts && rank[l * experts + e] < 0) rank[l * experts + e] = next++;
  }
  for (let i = 0; i < n; i++) if (rank[i] < 0) rank[i] = next++;
  return {layers, experts, rank};
}

// how many of each layer's experts the first `slots` ranks put in VRAM
export function vramPerLayer(p, slots) {
  const per = new Int32Array(p.layers);
  for (let i = 0; i < p.rank.length; i++) if (p.rank[i] < slots) per[(i / p.experts) | 0]++;
  return per;
}

// the hit rate over several requests, weighted by the tokens each wrote (the lookups follow the tokens)
export function meanHitRate(requests) {
  let w = 0, s = 0;
  for (const r of requests || []) if (r.hit_rate != null && r.output_tokens) { w += r.output_tokens; s += r.hit_rate * r.output_tokens; }
  return w ? s / w : null;
}
