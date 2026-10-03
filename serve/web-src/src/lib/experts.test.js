import {test, expect} from "bun:test";
import {readFileSync} from "node:fs";
import {decodeProfile, vramPerLayer, tokensPerPass} from "./experts.js";

const b64 = (pairs) => Buffer.from(new Uint16Array(pairs.flat()).buffer).toString("base64");

test("ranks follow the profile; pairs it does not name come after, in order", () => {
  const p = decodeProfile({layers: 2, experts: 3, ranked: b64([[1, 2], [0, 1]])});
  expect([...p.rank]).toEqual([2, 1, 3, 4, 5, 0]);
  expect([...p.order]).toEqual([1, 0, 2, 2, 0, 1]);
});
test("a repeated or out-of-range pair is skipped", () => {
  const p = decodeProfile({layers: 1, experts: 2, ranked: b64([[0, 1], [0, 1], [3, 0]])});
  expect([...p.rank]).toEqual([1, 0]);
});
test("the VRAM share per layer counts the first slots", () => {
  const p = decodeProfile({layers: 2, experts: 3, ranked: b64([[1, 2], [0, 1], [1, 0]])});
  expect([...vramPerLayer(p, 2)]).toEqual([1, 1]);
  expect([...vramPerLayer(p, 3)]).toEqual([1, 2]);
});
test("the shipped profile ranks all 48 x 512 experts", () => {
  const bin = readFileSync(new URL("../../../../data/expert-profile.bin", import.meta.url));
  const h = new Uint32Array(bin.buffer.slice(bin.byteOffset + 4, bin.byteOffset + 24));
  const p = decodeProfile({layers: h[1], experts: h[2], ranked: bin.subarray(24, 24 + h[4] * 4).toString("base64")});
  expect(p.rank.length).toBe(24576);
  expect(new Set(p.rank).size).toBe(24576);
  expect(vramPerLayer(p, 8980).reduce((a, b) => a + b, 0)).toBe(8980);
});
test("tokens per pass come from the accepted drafts", () => {
  expect(tokensPerPass([{output_tokens: 300, drafts_accepted: 200}])).toBe(3);
  expect(tokensPerPass([{output_tokens: 10}])).toBe(1);
  expect(tokensPerPass([])).toBe(1);
});
