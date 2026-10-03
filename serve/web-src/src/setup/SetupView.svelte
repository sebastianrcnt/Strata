<script>
  import {server, auth, setKey} from "../lib/server.svelte.js";
  import {ui, setTheme} from "../lib/ui.svelte.js";
  import {fmt, gb} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Facts from "../ui/Facts.svelte";
  import Check from "../ui/Check.svelte";
  import SharedDefaults from "./SharedDefaults.svelte";

  // INFO cvec=project:4-44[:singleL] | add:A-B | 0
  function projectionText(c) {
    if (!c || c === "0" || c === 0) return null;
    const [mode, range, single] = String(c).split(":");
    const [a, b] = (range || "").split("-");
    return `${mode === "project" ? "Projection" : "Additive"} control vector on layers ${a}–${b}` +
           `${single ? ` (layer ${single.replace("single", "")}'s direction)` : ""}. Per chat in its settings. Its package ` +
           "describes the vector as a refusal-direction projection; measure the speed yourself";
  }
  const m = $derived(server.metrics || {});
  const eng = $derived(m.engine || {}), hw = $derived(m.hardware || {}), st = $derived(m.hardware_static || {});
  const kv = $derived({int8: "8-bit", q4_0: "4-bit (Hadamard-rotated)", fp16: "16-bit"}[eng.kv] || eng.kv);
  const base = location.origin + location.pathname.replace(/\/$/, "");
  let key = $state(auth.key);
</script>

<div class="setup">
  <Panel title="Model and engine" lamp={server.health.loaded ? "on" : ""}>
    <Facts rows={[
      ["Model", eng.model],
      ["Engine", eng.version ? `v${eng.version}` : "built from source"],
      ["Context", eng.max_context ? `${fmt(eng.max_context)} tokens` : null],
      ["KV cache", kv ? `${kv}${eng.kv_resident ? `, streamed: ${fmt(eng.kv_resident)} positions per layer in VRAM, the rest in RAM` : ", all in VRAM"}` : null],
      ["Experts in VRAM", eng.expert_slots ? `${fmt(eng.expert_slots)} (${gb((eng.expert_cache_mib || 0) * 1048576)} GB)` : null],
      ["Speculation", eng.spec ? `MTP drafts up to ${Math.max(0, (eng.mtp_max || eng.spec) - 1)} tokens${eng.lookup ? ", prompt lookup on" : ""}` : null],
      ["Images", eng.images ? "on" : "off"],
      ["Experimental speed projection", projectionText(eng.cvec)],
    ]} />
  </Panel>
  <Panel title="This PC" lamp={null}>
    <Facts rows={[
      ["GPU", st.gpu_name ? `${st.gpu_name}${hw.gpu_mem_total ? `, ${gb(hw.gpu_mem_total, 0)} GB` : ""}` : "not readable (NVML)"],
      ["CPU", st.cpu_name ? `${st.cpu_name}${st.threads ? `, ${st.threads} threads` : ""}` : null],
      ["RAM", hw.ram_total ? `${gb(hw.ram_total, 0)} GB` : null],
    ]} />
  </Panel>
  <Panel title="Connect your tools" lamp={null}>
    <p class="muted">Any OpenAI- or Anthropic-compatible client works with these addresses.</p>
    <Facts rows={[["OpenAI base URL", `${base}/v1`, true], ["Anthropic base URL", base, true], ["Model name", eng.model || server.health.model, true]]} />
  </Panel>
  <SharedDefaults />
  <Panel title="This browser" lamp={null}>
    <div class="field">
      <label for="api-key">API key <span class="muted small">only if the server was started with one</span></label>
      <input class="input" class:needed={auth.needed} id="api-key" type="password" autocomplete="off" placeholder="not needed"
             bind:value={key} onchange={() => setKey(key)}>
    </div>
    <Check id="dark-theme" checked={ui.theme === "dark"} onchange={(v) => setTheme(v ? "dark" : "light", true)} label="Dark theme" />
    <p class="muted small">Chats, settings and the key are kept in this browser only.
      <a href="https://github.com/Niko1221/Strata" target="_blank" rel="noopener noreferrer">Strata on GitHub</a></p>
  </Panel>
</div>

<style>
  .setup { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: var(--pgap); padding: var(--pgap); align-items: start; }
  p { margin: 0 0 8px; }
  .field { display: flex; flex-direction: column; gap: 5px; margin-bottom: 12px; }
  .input { height: var(--ctl-h); padding: 0 7px; border: 0; border-radius: var(--r); background: var(--well); box-shadow: inset 0 0 0 1px var(--line); }
  .input:focus { outline: none; box-shadow: inset 0 0 0 1.5px var(--value); }
  .input.needed { box-shadow: inset 0 0 0 1.5px var(--accent); }
  p.small { margin: 12px 0 0; }
  @media (max-width: 800px) { .setup { grid-template-columns: 1fr; } }
</style>
