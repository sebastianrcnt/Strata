<script>
  // The MCP servers from the run config and the tools they offer the chat.
  import {server} from "../lib/server.svelte.js";
  import {fmt} from "../lib/format.js";

  const STATE = {ready: ["st-badge--ok", "Connected"], starting: ["st-badge--reading", "Starting"],
                 failed: ["st-badge--error", "Failed"], stopped: ["", "Stopped"], idle: ["", "Waiting"]};
  const servers = $derived(server.mcp.servers || []);
  const ready = $derived(servers.filter((s) => s.status === "ready" || s.status === "stopped"));
</script>

{#if servers.length}
  <div class="st-card mcp">
    <div class="panel-head"><span class="card-title">MCP servers</span>
      <span class="muted small">{fmt(server.mcp.tools)} tools · {ready.length} of {servers.length} servers connected</span></div>
    <div class="list">
      {#each servers as s (s.name)}
        {@const st = STATE[s.status] || ["", s.status]}
        <div class="server">
          <div class="server__head"><span class="st-badge {st[0]}">{st[1]}</span><strong>{s.name}</strong>
            <span class="muted small">{s.transport} · {fmt(s.tools.length)} tools{s.info?.name ? ` · ${s.info.name}${s.info.version ? ` ${s.info.version}` : ""}` : ""}</span></div>
          {#if s.error}<div class="msg-error">{s.error}</div>{/if}
          {#if s.tools.length}
            <div class="tools">{#each s.tools as t}<span class="chip" title={t.description || ""}>{t.tool}</span>{/each}</div>
          {/if}
        </div>
      {/each}
    </div>
    <p class="panel-note">Their tools run on this PC with your rights, when the model decides to call them
      (in this page's chat only; switch it off in the chat's settings).</p>
  </div>
{/if}

<style>
  .mcp { padding: 0; border: 0; }
  .list { padding: 6px 8px; display: flex; flex-direction: column; gap: 8px; }
  .server { display: flex; flex-direction: column; gap: 6px; }
  .server + .server { padding-top: 8px; border-top: 1px solid var(--st-line-soft); }
  .server__head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; font-size: 12px; }
  .tools { display: flex; flex-wrap: wrap; gap: 6px; }
  .tools .chip { height: 22px; padding: 0 8px; font-family: var(--st-font-mono); }
</style>
