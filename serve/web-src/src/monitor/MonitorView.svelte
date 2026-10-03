<script>
  import {monitor} from "../lib/server.svelte.js";
  import NowCard from "./NowCard.svelte";
  import Tracks from "./Tracks.svelte";
  import ContextCard from "./ContextCard.svelte";
  import RequestsCard from "./RequestsCard.svelte";
  import ConversationCard from "./ConversationCard.svelte";
  import McpCard from "./McpCard.svelte";

  const m = $derived(monitor.view);
</script>

<div class="monitor">
  <NowCard metrics={m} />
  <Tracks metrics={m} />
  <div class="row row--requests">
    <ContextCard metrics={m} />
    <RequestsCard metrics={m} />
  </div>
  <div class="row row--cards">
    <ConversationCard c={m?.conversations} />
    <McpCard />
  </div>
</div>

<style>
  .monitor { display: flex; flex-direction: column; gap: 1px; padding: 1px; background: var(--st-line); min-height: 100%;
             align-content: start; }
  .row { display: grid; gap: 1px; }
  .row--requests { grid-template-columns: 220px minmax(0, 1fr); }
  .row--cards { grid-template-columns: repeat(auto-fit, minmax(min(100%, 420px), 1fr)); flex: 1; align-items: start; }
  .row--cards:empty { display: none; }
  @media (max-width: 800px) { .row--requests { grid-template-columns: 1fr; } }
</style>
