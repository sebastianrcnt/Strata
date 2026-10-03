<script>
  // Server-wide sampling defaults (GET/POST /settings): every API client (omp, Pi, scripts...) gets these for
  // anything it does not set itself. Separate from the chat's own settings, and off unless switched on here.
  import {headers, projectionLoaded} from "../lib/server.svelte.js";
  import {toast} from "../lib/ui.svelte.js";
  import {DEFAULTS} from "../lib/chat.svelte.js";
  import {fmt} from "../lib/format.js";
  import SamplingFields from "../components/SamplingFields.svelte";

  let shared = $state(null);            // {shared, defaults} from the server; null until read
  let editing = $state(false);
  let draft = $state({...DEFAULTS});

  async function load() {
    try {
      const r = await fetch("settings", {headers: headers()});
      if (r.ok) shared = await r.json();
    } catch (e) { /* an older server: no shared settings */ }
  }
  load();

  // the server's form <-> the form's fields
  function fromServer(d) {
    return {...DEFAULTS, thinking: d.reasoning_effort ?? DEFAULTS.thinking, temperature: d.temperature ?? DEFAULTS.temperature,
            top_p: d.top_p ?? DEFAULTS.top_p, top_k: d.top_k ?? DEFAULTS.top_k, max: d.max_tokens ?? "", seed: d.seed ?? "",
            esp: d.experimental_speed_projection !== false};
  }
  function toServer(s) {
    const d = {reasoning_effort: s.thinking, temperature: +s.temperature};
    if (+s.temperature > 0) Object.assign(d, {top_p: +s.top_p, top_k: +s.top_k});
    if (s.seed) d.seed = +s.seed;
    if (s.max) d.max_tokens = +s.max;
    if (projectionLoaded()) d.experimental_speed_projection = s.esp !== false;
    return d;
  }
  async function save(defaults) {
    try {
      const r = await fetch("settings", {method: "POST", headers: headers(true), body: JSON.stringify({defaults})});
      if (!r.ok) {
        let msg = `HTTP ${r.status}`;
        try { msg = (await r.json()).error.message || msg; } catch (e) { /* not json */ }
        throw new Error(msg);
      }
      shared = await r.json();
      editing = false;
      toast("success", defaults ? "Defaults set for every app" : "Defaults removed",
            defaults ? "Apps that don't set these themselves use them from their next request." : "Every app uses its own settings again.");
    } catch (e) {
      toast("error", "Not saved", e.message, 6000);
    }
  }
  const edit = () => { draft = fromServer(shared?.shared ? shared.defaults || {} : {}); editing = true; };
  const LABELS = {reasoning_effort: "Thinking", temperature: "Temperature", top_p: "Top-p", top_k: "Top-k", max_tokens: "Max tokens",
                  seed: "Seed", experimental_speed_projection: "Speed projection"};
  const summary = $derived(Object.entries(shared?.defaults || {}).map(([k, v]) => `${LABELS[k] || k} ${typeof v === "number" ? fmt(v, v % 1 ? 2 : 0) : v}`).join(" · "));
</script>

<div class="st-card card">
  <div class="head">
    <span class="card-title">Defaults for other apps</span>
    {#if shared}<span class="st-badge {shared.shared ? 'st-badge--queued' : ''}">{shared.shared ? "On" : "Off"}</span>{/if}
  </div>
  <p class="muted">Sampling that every API client (omp, Pi, scripts, benchmarks) gets for anything it does not set itself.
    Changing this changes their results: leave it off while measuring.</p>
  {#if shared === null}
    <p class="muted small">Not available on this server.</p>
  {:else if !editing}
    {#if shared.shared}<p class="current">{summary}</p>{/if}
    <div class="actions">
      <button class="st-btn st-btn--secondary" onclick={edit}>{shared.shared ? "Change" : "Set defaults…"}</button>
      {#if shared.shared}<button class="st-btn st-btn--secondary" onclick={() => save(null)}>Turn off</button>{/if}
    </div>
  {:else}
    <div class="fields"><SamplingFields bind:s={draft} projection={projectionLoaded()} id="shared" /></div>
    <div class="actions">
      <button class="st-btn st-btn--secondary" onclick={() => (editing = false)}>Cancel</button>
      <button class="st-btn st-btn--primary" onclick={() => save(toServer(draft))}>Apply to every app</button>
    </div>
  {/if}
</div>

<style>
  .card { display: flex; flex-direction: column; gap: 10px; padding: 10px 12px; border: 0; }
  .head { display: flex; align-items: center; gap: 8px; }
  p { margin: 0; font-size: 12px; }
  .current { font-size: 12px; color: var(--st-ink-soft); }
  .fields { display: flex; flex-direction: column; gap: 16px; max-width: 420px; }
  .actions { display: flex; gap: 8px; }
  .actions .st-btn { height: 30px; padding: 0 12px; }
</style>
