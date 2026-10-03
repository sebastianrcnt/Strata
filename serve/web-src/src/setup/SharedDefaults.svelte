<script>
  // Server-wide sampling defaults (GET/POST /settings): every API client (omp, Pi, scripts...) gets these for
  // anything it does not set itself. Separate from the chat's own settings, and off unless switched on here.
  import {headers, projectionLoaded} from "../lib/server.svelte.js";
  import {toast} from "../lib/ui.svelte.js";
  import {DEFAULTS} from "../lib/chat.svelte.js";
  import {fmt} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Button from "../ui/Button.svelte";
  import Badge from "../ui/Badge.svelte";
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

<Panel title="Defaults for other apps" lamp={shared?.shared ? "on" : ""}>
  {#snippet tools()}{#if shared}<Badge tone={shared.shared ? "warn" : "neutral"}>{shared.shared ? "On" : "Off"}</Badge>{/if}{/snippet}
  <p class="muted">Sampling that every API client (omp, Pi, scripts, benchmarks) gets for anything it does not set itself.
    Changing this changes their results: leave it off while measuring.</p>
  {#if shared === null}
    <p class="muted">Not available on this server.</p>
  {:else if !editing}
    {#if shared.shared}<p>{summary}</p>{/if}
    <div class="actions">
      <Button onclick={edit}>{shared.shared ? "Change" : "Set defaults…"}</Button>
      {#if shared.shared}<Button onclick={() => save(null)}>Turn off</Button>{/if}
    </div>
  {:else}
    <div class="fields"><SamplingFields bind:s={draft} projection={projectionLoaded()} id="shared" /></div>
    <div class="actions">
      <Button onclick={() => (editing = false)}>Cancel</Button>
      <Button variant="primary" onclick={() => save(toServer(draft))}>Apply to every app</Button>
    </div>
  {/if}
</Panel>

<style>
  p { margin: 0 0 8px; }
  .fields { display: flex; flex-direction: column; gap: 14px; max-width: 420px; margin: 4px 0 12px; }
  .actions { display: flex; gap: 4px; }
</style>
