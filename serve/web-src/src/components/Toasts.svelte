<script>
  import {ui, dismiss} from "../lib/ui.svelte.js";
  import Button from "../ui/Button.svelte";
</script>

<div class="toasts" aria-live="polite">
  {#each ui.toasts as t (t.id)}
    <div class="toast toast--{t.kind}">
      <span class="toast__lamp" aria-hidden="true"></span>
      <div class="toast__text"><strong>{t.title}</strong>{#if t.text}<span>{t.text}</span>{/if}</div>
      {#if t.action}<Button class="toast-action" onclick={() => { t.action.run(); dismiss(t.id); }}>{t.action.label}</Button>{/if}
    </div>
  {/each}
</div>

<style>
  .toasts { position: fixed; right: 12px; bottom: 12px; z-index: 80; display: flex; flex-direction: column; gap: 6px; }
  .toast { display: flex; align-items: flex-start; gap: 8px; max-width: 380px; padding: 8px 10px; border-radius: var(--r);
           background: var(--head); box-shadow: var(--shadow); animation: toast-in 160ms ease-out; }
  .toast__lamp { flex: none; width: 8px; height: 8px; margin-top: 4px; border-radius: 50%; background: var(--value); }
  .toast--warn .toast__lamp { background: var(--accent); }
  .toast--error .toast__lamp { background: var(--danger); }
  .toast__text { display: flex; flex-direction: column; gap: 1px; font-size: var(--fs-s); color: var(--dim); }
  .toast__text strong { font-size: var(--fs-m); font-weight: var(--fw-b); color: var(--text); }
  @keyframes toast-in { from { opacity: 0; transform: translateY(4px); } }
  @media (max-width: 640px) { .toasts { left: 8px; right: 8px; } .toast { max-width: none; } }
</style>
