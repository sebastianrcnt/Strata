// The expert profile for the Memory tab. data/expert-profile.bin ranks every (layer, expert) pair of the model, most
// used first; the engine fills its VRAM expert cache with the first `expert_slots` pairs when it starts, and every
// expert also sits in RAM (the arena). The engine then swaps a few experts toward the conversation (--adapt-swaps)
// without reporting which, so what this gives is the placement at start.

// {layers, experts, ranked: base64 of uint16 (layer, expert) pairs} -> per layer, its experts best first
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
  for (let i = 0; i < n; i++) if (rank[i] < 0) rank[i] = next++;   // a short profile: the rest after it, in order
  // order[l * experts + k]: layer l's k-th best expert
  const order = new Uint16Array(n);
  for (let l = 0; l < layers; l++) {
    const ids = Array.from({length: experts}, (_, e) => e).sort((a, b) => rank[l * experts + a] - rank[l * experts + b]);
    order.set(ids, l * experts);
  }
  return {layers, experts, rank, order};
}

// how many of each layer's experts the first `slots` ranks put in VRAM
export function vramPerLayer(p, slots) {
  const per = new Int32Array(p.layers);
  for (let i = 0; i < p.rank.length; i++) if (p.rank[i] < slots) per[(i / p.experts) | 0]++;
  return per;
}

// tokens each pass over the 48 layers produces (the MTP drafts accepted ride along): from the recent requests
export function tokensPerPass(requests) {
  let out = 0, acc = 0;
  for (const r of requests || []) if (r.output_tokens && r.drafts_accepted != null) { out += r.output_tokens; acc += r.drafts_accepted; }
  return out > acc && acc > 0 ? out / (out - acc) : 1;
}
