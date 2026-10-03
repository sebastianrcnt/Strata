<script>
  import {ui, dismiss} from "../lib/ui.svelte.js";
  import Icon from "./Icon.svelte";
  const ICONS = {info: "info", success: "check", warn: "warning", error: "error"};
</script>

<div class="st-toasts" aria-live="polite">
  {#each ui.toasts as t (t.id)}
    <div class="st-toast st-toast--{t.kind}">
      <Icon name={ICONS[t.kind] || "info"} />
      <div><div class="st-toast__title">{t.title}</div><div>{t.text}</div></div>
      {#if t.action}
        <button class="st-btn st-btn--secondary toast-action" onclick={() => { t.action.run(); dismiss(t.id); }}>{t.action.label}</button>
      {/if}
    </div>
  {/each}
</div>

<style>
  .st-toast { animation: toast-in 180ms var(--st-ease); }
  .toast-action { height: 32px; margin-left: auto; }
  @keyframes toast-in { from { opacity: 0; transform: translateY(6px); } }
  @media (max-width: 640px) { .st-toasts { left: 12px; right: 12px; bottom: 12px; } }
</style>
