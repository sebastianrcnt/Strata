<script>
  import {monitor} from "../lib/server.svelte.js";
  import NowCard from "./NowCard.svelte";
  import Tracks from "./Tracks.svelte";
  import ContextCard from "./ContextCard.svelte";
  import RequestsCard from "./RequestsCard.svelte";
  import ConversationCard from "./ConversationCard.svelte";

  const m = $derived(monitor.view);
</script>

<!-- Wide screens: the tracks over two columns (context beside the requests and the cache). Phones: one column,
     the requests right after "now", the long tracks after them. -->
<div class="monitor">
  <div class="slot slot--now"><NowCard metrics={m} /></div>
  <div class="slot slot--tracks"><Tracks metrics={m} /></div>
  <div class="cols">
    <div class="col col--side">
      <div class="slot slot--context"><ContextCard metrics={m} /></div>
    </div>
    <div class="col col--main">
      <div class="slot slot--requests"><RequestsCard metrics={m} /></div>
      <div class="slot slot--cache"><ConversationCard c={m?.conversations} /></div>
    </div>
  </div>
</div>

<style>
  .monitor { display: flex; flex-direction: column; gap: var(--pgap); padding: var(--pgap); }
  .monitor > *, .col > * { flex: none; }
  .cols { display: grid; grid-template-columns: 230px minmax(0, 1fr); gap: var(--pgap); align-items: start; }
  .col { display: flex; flex-direction: column; gap: var(--pgap); min-width: 0; }
  .slot:empty { display: none; }
  @media (max-width: 800px) {
    .cols, .col { display: contents; }
    .slot--now { order: 1; }
    .slot--requests { order: 2; }
    .slot--tracks { order: 3; }
    .slot--context { order: 4; }
    .slot--cache { order: 5; }
  }
</style>
