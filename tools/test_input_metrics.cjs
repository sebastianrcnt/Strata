// node --test tools/test_input_metrics.cjs (no GPU/server required)
const {test} = require('node:test');
const assert = require('node:assert/strict');
const input = require('../serve/web/input-metrics.js');
const fmt = (n, d = 0) => n == null ? '–' : n.toLocaleString('en-US', {maximumFractionDigits: d, minimumFractionDigits: d});
const request = {prompt_tokens: 3270, reused: 2521, prompt_ms: 2468};
test('effective rate excludes cache hits and pairs the rate with its own elapsed time', () => {
  const s = input.summary(request);
  assert.equal(s.fresh, 749);
  assert.ok(Math.abs(s.rate - 303.4846) < .001);
  assert.equal(input.card({state: 'idle'}, request, fmt).detail, '2.47 s input prep · 749 new · 2,521 reused');
});
test('missing, zero and invalid timings do not become fabricated throughput', () => {
  for (const r of [null, {}, {...request, reused: null}, {...request, prompt_ms: null},
    {...request, prompt_ms: 0}, {...request, prompt_ms: Infinity}, {...request, prompt_ms: -1},
    {...request, prompt_tokens: NaN}, {...request, reused: 3270}, {...request, reused: 4000}]) {
    assert.equal(input.summary(r).rate, null);
  }
  assert.equal(input.summary({...request, reused: 0}).fresh, 3270);
});
test('live estimates never borrow final counts from an unrelated previous request', () => {
  for (const state of ['reading', 'generating']) {
    const c = input.card({state, prefill_tok_s_mean: 1000}, request, fmt);
    assert.equal(c.rate, 1000);
    assert.match(c.label, /provisional/);
    assert.doesNotMatch(c.detail, /749|2,521|2.47/);
    assert.equal(input.card({state}, request, fmt).rate, null);
  }
});
test('unloaded and empty history do not show stale rates', () => {
  assert.equal(input.card({state: 'unloaded'}, request, fmt).rate, null);
  assert.equal(input.card({state: 'idle'}, null, fmt).rate, null);
});
