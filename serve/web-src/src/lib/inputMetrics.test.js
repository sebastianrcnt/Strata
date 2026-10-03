// bun test (no GPU/server required); moved from tools/test_input_metrics.cjs
import {test, expect} from "bun:test";
import * as input from "./inputMetrics.js";

const fmt = (n, d = 0) => n == null ? "–" : n.toLocaleString("en-US", {maximumFractionDigits: d, minimumFractionDigits: d});
const request = {prompt_tokens: 3270, reused: 2521, prompt_ms: 2468};

test("effective rate excludes cache hits and pairs the rate with its own elapsed time", () => {
  const s = input.summary(request);
  expect(s.fresh).toBe(749);
  expect(Math.abs(s.rate - 303.4846)).toBeLessThan(.001);
  expect(input.card({state: "idle"}, request, fmt).detail).toBe("2.47 s input prep · 749 new · 2,521 reused");
});
test("missing, zero and invalid timings do not become fabricated throughput", () => {
  for (const r of [null, {}, {...request, reused: null}, {...request, prompt_ms: null},
    {...request, prompt_ms: 0}, {...request, prompt_ms: Infinity}, {...request, prompt_ms: -1},
    {...request, prompt_tokens: NaN}, {...request, reused: 3270}, {...request, reused: 4000}]) {
    expect(input.summary(r).rate).toBeNull();
  }
  expect(input.summary({...request, reused: 0}).fresh).toBe(3270);
});
test("live estimates never borrow final counts from an unrelated previous request", () => {
  for (const state of ["reading", "generating"]) {
    const c = input.card({state, prefill_tok_s_mean: 1000}, request, fmt);
    expect(c.rate).toBe(1000);
    expect(c.label).toMatch(/provisional/);
    expect(c.detail).not.toMatch(/749|2,521|2.47/);
    expect(input.card({state}, request, fmt).rate).toBeNull();
  }
});
test("unloaded and empty history do not show stale rates", () => {
  expect(input.card({state: "unloaded"}, request, fmt).rate).toBeNull();
  expect(input.card({state: "idle"}, null, fmt).rate).toBeNull();
});
