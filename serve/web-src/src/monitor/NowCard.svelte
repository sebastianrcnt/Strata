<script>
  // What the engine is doing right now, for any client: the state, the input being read, and a short live tape of
  // the output. Prompts and output are not kept: only the server's bounded preview is shown. The two rows stay put
  // between requests (the page below does not jump) and keep the last request's preview, as this browser saw it,
  // until the next one starts.
  import {server, monitor} from "../lib/server.svelte.js";
  import {fmt, kfmt} from "../lib/format.js";
  import * as inputMetrics from "../lib/inputMetrics.js";
  import Panel from "../ui/Panel.svelte";
  import Badge from "../ui/Badge.svelte";
  import Value from "../ui/Value.svelte";

  let {metrics} = $props();

  const s = $derived(monitor.live ? server.stream : {state: "idle"});
  const live = $derived(metrics?.live || {});
  const last = $derived(metrics?.requests?.[0]);
  const reading = $derived(s.state === "reading");
  const generating = $derived(s.state === "generating");
  const queued = $derived(live.queued || 0);

  const PHASES = {thinking: "Thinking", answering: "Answering"};
  const title = $derived(reading ? "Reading the input"
                         : generating ? (PHASES[s.phase] || (s.phase ? s.phase[0].toUpperCase() + s.phase.slice(1) : "Generating"))
                         : live.state === "unloaded" ? "Unloaded" : monitor.live ? "Idle" : "Paused");
  const known = $derived(reading && s.prompt_total > 0 && s.prompt_read != null);
  // the input bar shows only how much of the input is read (a new request starts it from zero, without animating)
  const busy = $derived(reading || generating);
  const readPct = $derived(known ? Math.min(100, (100 * s.prompt_read) / s.prompt_total) : generating || (!busy && last) ? 100 : 0);
  // between requests: the last one's input, from its record
  const lastInput = $derived.by(() => {
    if (busy || !last) return "–";
    const i = inputMetrics.summary(last);
    return `${kfmt(last.prompt_tokens)}` + (i.reused != null && last.prompt_tokens ? ` · ${fmt((100 * i.reused) / last.prompt_tokens)}% cached` : "");
  });
  // the last input preview this browser saw, kept until the next request
  let prompt = $state("");
  $effect(() => { if (busy && s.prompt_preview) prompt = previewUnicode(s.prompt_preview); });

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
    if (st.state !== "reading" && st.state !== "generating") {   // between requests: keep the last tape, settled
      cancelAnimationFrame(frame);
      if (pending) shown = (shown + pending).slice(-4096);
      frame = 0; tick = 0; tail = ""; pending = ""; fresh = "";
      return;
    }
    if (st.request !== request) {                                  // a new request: a new tape
      cancelAnimationFrame(frame);
      frame = 0; tick = 0; tail = ""; pending = ""; shown = ""; fresh = ""; request = st.request;
      if (st.state === "reading") prompt = "";
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

  let openInput = $state(false), openOutput = $state(false);
  let outputPanel = $state();
  $effect.pre(() => {
    shown;
    if (!outputPanel) return;
    const follow = outputPanel.scrollHeight - outputPanel.scrollTop - outputPanel.clientHeight < 24;
    if (follow) queueMicrotask(() => { if (outputPanel) outputPanel.scrollTop = outputPanel.scrollHeight; });
  });
</script>

<Panel {title} lamp={reading ? "value" : generating ? "on" : ""} flush aria-label="What the model is doing now">
  {#snippet tools()}
    {#if server.streamStatus}<Badge tone="error">{server.streamStatus}</Badge>{/if}
    {#if queued > 0}<Badge tone="warn">{queued} queued</Badge>{/if}
    {#if generating}
      <Value value={live.max_tokens ? `${fmt(s.generated)} / ${fmt(live.max_tokens)}` : fmt(s.generated)} unit="tokens" />
      <Value value={s.tok_s == null ? null : fmt(s.tok_s, 1)} unit="tok/s" active />
    {:else if known}
      <Value value={`${fmt(s.prompt_read)} / ${fmt(s.prompt_total)}`} unit="tokens" active />
    {:else if !reading && last}
      <span>last: {fmt(last.output_tokens)} tokens{last.decode_tok_s ? ` at ${fmt(last.decode_tok_s, 1)} tok/s` : ""}</span>
    {/if}
  {/snippet}
  <div class="row">
    <button class="row__label" aria-expanded={openInput} onclick={() => (openInput = !openInput)}><span class="tri" class:open={openInput}></span>Input</button>
    {#key s.request}
      <div class="bar" class:indeterminate={reading && !known} class:settled={!busy}><i style:width="{readPct}%"></i></div>
    {/key}
    <span class="row__end">{known ? `${kfmt(s.prompt_read)} / ${kfmt(s.prompt_total)}` : reading ? (s.prompt_tokens ? `${kfmt(s.prompt_tokens)} tokens` : "preparing") : generating ? "read" : lastInput}</span>
  </div>
  {#if openInput}
    <pre class="text">{busy ? previewUnicode(s.prompt_preview || "Preparing input…") : prompt || "Waiting for a request"}</pre>
  {/if}

  <div class="row">
    <button class="row__label" aria-expanded={openOutput} onclick={() => (openOutput = !openOutput)}><span class="tri" class:open={openOutput}></span>Output</button>
    <div class="tape-window">
      <div class="tape" class:settled={!generating}>
        {#if generating}<span>{tapeOld}</span><span class="tape__new">{tapeNew}</span><i class="tape__cursor"></i>
        {:else if reading}<span>waiting for the input to be read</span>
        {:else if shown}<span>{tapeOld}{tapeNew}</span>
        {:else}<span>waiting for a request</span>{/if}
      </div>
    </div>
    <span class="row__end"></span>
  </div>
  {#if openOutput}
    <pre class="text" bind:this={outputPanel}>{generating || (!reading && shown) ? shown : reading ? "Waiting for the input to be read" : "Waiting for a request"}</pre>
  {/if}
</Panel>

<style>
  .bar { height: 3px; border-radius: 2px; background: var(--well); overflow: hidden; }
  .bar i { display: block; height: 100%; background: var(--value); transition: width 180ms linear; }
  .bar.settled i { background: var(--off); }
  .bar.indeterminate i { width: 30% !important; animation: slide 1.2s linear infinite; }
  .row { display: grid; grid-template-columns: 72px minmax(0, 1fr) auto; align-items: center; gap: 8px; height: 28px; padding: 0 10px 0 6px; }
  .row { border-top: 1px solid var(--gap); }
  .row__label { display: flex; align-items: center; gap: 6px; height: 100%; padding: 0; border: 0; background: none; color: var(--dim);
                font-size: var(--fs-s); cursor: pointer; }
  .row__label:hover { color: var(--text); }
  .tri { width: 0; height: 0; border-top: 4px solid transparent; border-bottom: 4px solid transparent; border-left: 5px solid currentColor;
         transition: transform 120ms; }
  .tri.open { transform: rotate(90deg); }
  .row__end { font-size: var(--fs-s); color: var(--dim); text-align: right; white-space: nowrap; }
  .tape-window { position: relative; min-width: 0; height: 20px; overflow: hidden;
                 mask-image: linear-gradient(to right, transparent, black 15%, black); }
  .tape { position: absolute; right: 0; top: 0; width: max-content; white-space: pre; font: var(--fs-m)/20px var(--mono); color: var(--dim); }
  .tape__new { color: var(--text); }
  .tape.settled, .tape.settled .tape__new { color: var(--off); }
  .tape__cursor { display: inline-block; width: 2px; height: 13px; margin-left: 3px; vertical-align: middle; background: var(--accent);
                  animation: blink .8s ease-in-out infinite alternate; }
  .text { margin: 0; height: 160px; overflow: auto; padding: 8px 10px; white-space: pre-wrap; overflow-wrap: anywhere;
          font: var(--fs-m)/1.55 var(--mono); background: var(--well); }
  @keyframes slide { from { transform: translateX(-100%); } to { transform: translateX(340%); } }
  @keyframes blink { to { opacity: .3; } }
  @media (max-width: 640px) { .row { grid-template-columns: 60px minmax(0, 1fr) auto; padding-right: 8px; } }
</style>
