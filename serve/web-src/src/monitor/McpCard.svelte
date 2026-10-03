<script>
  // The MCP servers from the run config and the tools they offer the chat.
  import {server} from "../lib/server.svelte.js";
  import {fmt} from "../lib/format.js";
  import Panel from "../ui/Panel.svelte";
  import Badge from "../ui/Badge.svelte";

  const STATE = {ready: ["value", "Connected"], starting: ["warn", "Starting"], failed: ["error", "Failed"], stopped: ["neutral", "Stopped"], idle: ["neutral", "Waiting"]};
  const servers = $derived(server.mcp.servers || []);
  const ready = $derived(servers.filter((s) => s.status === "ready" || s.status === "stopped"));
</script>

{#if servers.length}
  <Panel title="MCP servers" lamp={servers.some((s) => s.status === "failed") ? "error" : ready.length ? "value" : ""}>
    <p class="sum">{fmt(server.mcp.tools)} tools · {ready.length} of {servers.length} servers connected</p>
    {#each servers as s (s.name)}
      {@const st = STATE[s.status] || ["neutral", s.status]}
      <div class="server">
        <div class="server__head"><Badge tone={st[0]}>{st[1]}</Badge><strong>{s.name}</strong>
          <span class="muted">{s.transport} · {fmt(s.tools.length)} tools{s.info?.name ? ` · ${s.info.name}${s.info.version ? ` ${s.info.version}` : ""}` : ""}</span></div>
        {#if s.error}<div class="msg-error">{s.error}</div>{/if}
        {#if s.tools.length}<div class="tools">{#each s.tools as t}<span class="chip mono" title={t.description || ""}>{t.tool}</span>{/each}</div>{/if}
      </div>
    {/each}
    <p class="note">Their tools run on this PC with your rights, when the model decides to call them (in this page's chat only;
      switch it off in the chat's settings).</p>
  </Panel>
{/if}

<style>
  .server { display: flex; flex-direction: column; gap: 6px; }
  .server + .server { margin-top: 8px; padding-top: 8px; border-top: 1px solid var(--gap); }
  .server__head { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
  strong { font-weight: var(--fw-b); }
  .tools { display: flex; flex-wrap: wrap; gap: 4px; }
  .sum { margin: 0 0 8px; font-size: var(--fs-s); color: var(--dim); }
  .note { margin: 8px 0 0; font-size: var(--fs-s); color: var(--dim); }
</style>
