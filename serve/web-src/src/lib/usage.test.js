import {test, expect} from "bun:test";
import {decodeUsage, since, summarize} from "./usage.js";

// a report as the engine writes it: three uint32 arrays, then one residency bit per expert
function report(vram, gpu, cpu, held) {
  const n = vram.length, b = new Uint8Array(n * 12 + Math.ceil(n / 8));
  new Uint32Array(b.buffer, 0, n * 3).set([...vram, ...gpu, ...cpu]);
  held.forEach((h, i) => { if (h) b[n * 12 + (i >> 3)] |= 1 << (i & 7); });
  return Buffer.from(b).toString("base64");
}

test("a report decodes into its counts and residency", () => {
  const u = decodeUsage(report([5, 0, 1, 0], [0, 2, 0, 0], [0, 0, 3, 0], [1, 0, 1, 0]), 2, 2);
  expect([...u.vram]).toEqual([5, 0, 1, 0]);
  expect([...u.gpu]).toEqual([0, 2, 0, 0]);
  expect([...u.cpu]).toEqual([0, 0, 3, 0]);
  expect([...u.held]).toEqual([1, 0, 1, 0]);
  expect(decodeUsage("AAAA", 2, 2)).toBe(null);   // too short
});
test("the last request is now minus before", () => {
  const before = decodeUsage(report([1, 0, 0, 0], [0, 0, 0, 0], [0, 1, 0, 0], [1, 0, 0, 0]), 2, 2);
  const now = decodeUsage(report([4, 0, 0, 0], [0, 0, 0, 0], [0, 3, 0, 0], [1, 1, 0, 0]), 2, 2);
  const d = since(now, before);
  expect([...d.vram]).toEqual([3, 0, 0, 0]);
  expect([...d.cpu]).toEqual([0, 2, 0, 0]);
  expect([...d.held]).toEqual([1, 1, 0, 0]);
});
test("the summary: hit rate, per layer, the 90% set and the most missed", () => {
  const u = decodeUsage(report([80, 0, 0, 0], [0, 0, 5, 0], [0, 10, 5, 0], [1, 0, 0, 0]), 2, 2);
  const s = summarize(u, 2, 2);
  expect(s.hit).toBe(.8);
  expect(s.held).toBe(1);
  expect(s.used).toBe(3);
  expect(s.top90).toBe(2);                         // 80 + 10 of 100
  expect(s.perLayer.map((l) => l.hit)).toEqual([80 / 90, 0]);
  expect(s.missed.map((m) => [m.layer, m.expert, m.lookups])).toEqual([[0, 1, 10], [1, 0, 10]]);
});
