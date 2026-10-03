// The Monitor's graphs: the server keeps the last 60 one-second samples per series; the browser joins them into a
// longer record (up to `keep` seconds) by the time that passed, so a slow poll (a background tab) or a reload does
// not squeeze or lose time. Seconds nobody sampled become null (a gap in the line).

// stored: {t, series: {key: [...]}} (t = the newest sample's time in ms) or null; fresh: the server's {key: [...]}
export function mergeHistory(stored, fresh, now, keep = 300) {
  const keys = Object.keys(fresh || {});
  if (!stored || !stored.series || !(now - stored.t < keep * 1000)) {
    return {t: now, series: Object.fromEntries(keys.map((k) => [k, fresh[k].slice(-keep)]))};
  }
  const elapsed = Math.round((now - stored.t) / 1000);
  if (elapsed <= 0) return stored;
  const series = {};
  for (const k of new Set([...keys, ...Object.keys(stored.series)])) {
    const old = stored.series[k] || [], add = (fresh || {})[k] || [];
    const joined = elapsed >= add.length
      ? [...old, ...Array(Math.min(keep, elapsed - add.length)).fill(null), ...add]
      : [...old, ...add.slice(add.length - elapsed)];
    series[k] = joined.slice(-keep);
  }
  return {t: stored.t + elapsed * 1000, series};
}

// Between requests the engine reports a speed of 0; on the graph that is "not running", not "slow".
export const idleAsGap = (values) => (values || []).map((v) => (v === 0 ? null : v));
