// Presentation only: preserve the engine/API timing fields and exclude reused tokens.
export const explanation = "New tokens divided by input preparation time, including cache handling. " +
  "This is effective request throughput, not isolated GPU prefill speed.";
const valid = (n) => typeof n === "number" && Number.isFinite(n) && n >= 0;

export function summary(r) {
  const total = r && valid(r.prompt_tokens) ? r.prompt_tokens : null;
  const reused = r && valid(r.reused) ? r.reused : null;
  const fresh = total != null && reused != null ? Math.max(0, total - reused) : null;
  const ms = r && valid(r.prompt_ms) ? r.prompt_ms : null;
  return {fresh, reused, ms, rate: fresh != null && fresh > 0 && ms > 0 ? fresh * 1000 / ms : null};
}

export function card(live, last, fmt) {
  if (live.state === "reading" || live.state === "generating") {
    const rate = valid(live.prefill_tok_s_mean) ? live.prefill_tok_s_mean : null;
    return {rate, label: "Input effective · provisional",
      detail: "Current request: waiting for final input timing and cache counts."};
  }
  if (live.state !== "idle" || !last) return {rate: null, label: "Input effective", detail: "No completed input timing."};
  const s = summary(last);
  return {rate: s.rate, label: "Input effective · last request",
    detail: `${s.ms == null ? "–" : fmt(s.ms / 1000, 2) + " s"} input prep · ${fmt(s.fresh)} new · ${fmt(s.reused)} reused`};
}
