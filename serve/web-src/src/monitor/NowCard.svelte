<script>
  // What the engine is doing right now, for any client: the state, the input being read, and a short live tape of
  // the output. Prompts and output are not kept: only the server's bounded preview is shown.
  import {server, monitor} from "../lib/server.svelte.js";
  import {fmt, kfmt} from "../lib/format.js";

  let {metrics} = $props();

  const s = $derived(monitor.live ? server.stream : {state: "idle"});
  const live = $derived(metrics?.live || {});
  const last = $derived(metrics?.requests?.[0]);
  const reading = $derived(s.state === "reading");
  const generating = $derived(s.state === "generating");
  const queued = $derived(live.queued || 0);

  const PHASES = {thinking: "Thinking", answering: "Answering"};
  const badge = $derived(reading ? ["reading", "Reading"] : generating ? ["generating", "Generating"]
                         : live.state === "unloaded" ? ["", "Unloaded"] : ["", monitor.live ? "Idle" : "Paused"]);
  const label = $derived(reading ? "Reading the prompt"
                         : generating ? (PHASES[s.phase] || (s.phase ? s.phase[0].toUpperCase() + s.phase.slice(1) : "Generating"))
                         : "Waiting for a request");
  const known = $derived(reading && s.prompt_total > 0 && s.prompt_read != null);
  const detail = $derived(
    known ? `${fmt(s.prompt_read)} / ${fmt(s.prompt_total)} tokens · ${fmt((100 * s.prompt_read) / s.prompt_total)}%`
    : reading ? (s.prompt_tokens ? `${fmt(s.prompt_tokens)} tokens` : "Preparing")
    : generating ? `${fmt(s.generated)} tokens${s.tok_s != null ? ` · ${fmt(s.tok_s, 1)} tok/s` : ""}`
    : last ? `last: ${fmt(last.output_tokens)} tokens${last.decode_tok_s ? ` at ${fmt(last.decode_tok_s, 1)} tok/s` : ""}` : "");
  const pct = $derived(known ? (100 * s.prompt_read) / s.prompt_total
                       : generating && live.max_tokens ? Math.min(100, (100 * s.generated) / live.max_tokens) : 0);

  // ------------------------------------------------------------------ the output tape
  // The server sends the last 4,096 characters; new characters are let out over a few frames so the tape moves
  // smoothly at 5 polls a second.
  const previewUnicode = (text) => text.replace(/(?:\\u[0-9a-fA-F]{4})+/g, (part) => {
    try { return JSON.parse('"' + part + '"'); } catch (_) { return part; }
  });
  let shown = $state(""), fresh = $state("");
  let request = null, tail = "", pending = "", frame = 0, tick = 0;
  function animate(now) {
    frame = 0;
    if (document.hidden) { tick = 0; return; }
    const dt = Math.min(40, tick ? now - tick : 16); tick = now;
    const chars = Array.from(pending);
    const n = Math.min(chars.length, Math.max(1, Math.ceil(chars.length * dt / 180)));
    fresh = chars.slice(0, n).join("");
    pending = chars.slice(n).join("");
    shown = (shown + fresh).slice(-4096);
    if (pending) frame = requestAnimationFrame(animate); else tick = 0;
  }
  $effect(() => {
    const st = s;
    if (st.request !== request || st.state !== "generating") {
      cancelAnimationFrame(frame);
      frame = 0; tick = 0; tail = ""; pending = ""; shown = ""; fresh = ""; request = st.request;
    }
    if (st.state !== "generating") return;
    const t = previewUnicode(st.tail || "");
    if (t !== tail) {
      let overlap = Math.min(tail.length, t.length);
      while (overlap > 0 && !t.startsWith(tail.slice(-overlap))) overlap--;
      pending = (pending + t.slice(overlap)).slice(-4096); tail = t;
    }
    if (pending && !frame) frame = requestAnimationFrame(animate);
  });
  $effect(() => () => cancelAnimationFrame(frame));

  const flat = (x) => x.replace(/\s+/g, " ");
  const tapeNew = $derived(flat(fresh).slice(-80));
  const tapeOld = $derived.by(() => { const v = flat(shown).slice(-160); return v.slice(0, Math.max(0, v.length - tapeNew.length)); });

  let openPrefill = $state(false), openDecode = $state(false);
  let decodePanel = $state();
  $effect.pre(() => {
    shown;
    if (!decodePanel) return;
    const follow = decodePanel.scrollHeight - decodePanel.scrollTop - decodePanel.clientHeight < 24;
    if (follow) queueMicrotask(() => { if (decodePanel) decodePanel.scrollTop = decodePanel.scrollHeight; });
  });
</script>

<section class="now st-card" aria-label="What the model is doing now">
  <div class="now__head">
    <span class="st-badge {badge[0] ? `st-badge--${badge[0]}` : ''}">{badge[1]}</span>
    {#if queued > 0}<span class="st-badge st-badge--queued">{queued} queued</span>{/if}
    <span class="now__label">{label}</span>
    <span class="now__detail muted">{detail}</span>
    {#if server.streamStatus}<span class="now__status st-badge st-badge--queued">{server.streamStatus}</span>{/if}
  </div>
  <div class="st-progress" data-tone={reading ? "info" : undefined}><div class="st-progress__bar" style:width="{pct}%"></div></div>

  <div class="now__row">
    <button class="now__expand" aria-expanded={openPrefill} onclick={() => (openPrefill = !openPrefill)}>{openPrefill ? "▾" : "▸"} Input</button>
    <div class="prefill" class:indeterminate={reading && !known}>
      <i style:width={known ? `${Math.min(100, pct)}%` : generating ? "100%" : "0%"}></i>
    </div>
    <span class="now__count">{known ? `${kfmt(s.prompt_read)} / ${kfmt(s.prompt_total)}` : reading ? "Reading" : generating ? "Done" : "–"}</span>
  </div>
  {#if openPrefill}
    <pre class="now__text">{reading || generating ? previewUnicode(s.prompt_preview || "Preparing input…") : "Waiting for a request"}</pre>
  {/if}

  <div class="now__row">
    <button class="now__expand" aria-expanded={openDecode} onclick={() => (openDecode = !openDecode)}>{openDecode ? "▾" : "▸"} Output</button>
    <div class="tape-window">
      <div class="tape">
        {#if generating}<span>{tapeOld}</span><span class="tape__new">{tapeNew}</span><i class="tape__cursor"></i>
        {:else}<span>{reading ? "Waiting for the input to be read" : "Waiting for a request"}</span>{/if}
      </div>
    </div>
    <span class="now__count">{generating && s.tok_s != null ? `${fmt(s.tok_s, 1)} t/s` : "–"}</span>
  </div>
  {#if openDecode}
    <pre class="now__text" bind:this={decodePanel}>{generating ? shown : "Waiting for a request"}</pre>
  {/if}
</section>

<style>
  .now { padding: 0; border: 0; font-size: 12px; }
  .now__head { display: flex; align-items: center; flex-wrap: wrap; gap: 6px 10px; padding: 7px 10px; }
  .now__label { font-weight: var(--st-fw-medium); }
  .now__detail { font-size: 11px; }
  .now__status { margin-left: auto; }
  .now__row { display: grid; grid-template-columns: 72px minmax(0, 1fr) auto; align-items: center; gap: 9px; padding: 6px 10px;
              min-height: 36px; border-top: 1px solid var(--st-line-soft); }
  .now__expand { background: none; border: 0; padding: 0; text-align: left; font: 11px var(--st-font); color: var(--st-ink-muted);
                 cursor: pointer; white-space: nowrap; }
  .now__expand:hover { color: var(--st-ink); }
  .now__count { font-size: 11px; min-width: 68px; text-align: right; white-space: nowrap; }
  .prefill { height: 3px; background: var(--st-surface-2); overflow: hidden; }
  .prefill i { display: block; height: 100%; width: 0; background: var(--st-info); transition: width 180ms linear; }
  .prefill.indeterminate i { width: 35% !important; animation: read 1.2s linear infinite; }
  .tape-window { position: relative; min-width: 0; overflow: hidden; height: 22px;
                 mask-image: linear-gradient(to right, transparent, black 18%, black); }
  .tape { position: absolute; right: 0; top: 0; width: max-content; white-space: pre; font: 13px/22px var(--st-font-mono);
          color: var(--st-ink-muted); }
  .tape__new { color: var(--st-ink); }
  .tape__cursor { display: inline-block; width: 2px; height: 15px; background: var(--st-accent); vertical-align: middle; margin-left: 4px;
                  animation: cursor .8s ease-in-out infinite alternate; }
  .now__text { box-sizing: border-box; margin: 0; padding: 10px 12px; height: 164px; overflow: auto; white-space: pre-wrap;
               overflow-wrap: anywhere; font: 12px/1.6 var(--st-font-mono); border-top: 1px solid var(--st-line-soft);
               background: var(--st-bg); color: var(--st-ink); }
  @keyframes read { from { transform: translateX(-100%); } to { transform: translateX(390%); } }
  @keyframes cursor { to { opacity: .35; } }
  @media (max-width: 640px) {
    .now__row { grid-template-columns: 56px minmax(0, 1fr) auto; gap: 6px; padding: 6px 8px; }
    .now__count { min-width: 0; }
  }
</style>
