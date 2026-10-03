// Page-level state: the tab, the theme and the toasts.
import {store} from "./storage.js";

export const TABS = ["chat", "monitor", "memory", "about"];
export const ui = $state({tab: "monitor", theme: document.documentElement.dataset.theme || "dark", toasts: []});

// ------------------------------------------------------------------ tabs (the URL hash keeps the tab across reloads)
export function showTab(name, chatOn) {
  const tab = TABS.includes(name) && (name !== "chat" || chatOn) ? name : chatOn ? "chat" : "monitor";
  ui.tab = tab;
  const hash = tab === (chatOn ? "chat" : "monitor") ? "" : `#${tab}`;
  if (location.hash !== hash) history.replaceState(null, "", location.pathname + location.search + hash);
}

// ------------------------------------------------------------------ theme: the system's until the user picks one
export function setTheme(t, save) {
  ui.theme = t;
  document.documentElement.dataset.theme = t;
  if (save) try { localStorage.setItem("strata.theme", t); } catch (e) { /* ignore */ }
}
export const flipTheme = () => setTheme(ui.theme === "dark" ? "light" : "dark", true);
matchMedia("(prefers-color-scheme: dark)").addEventListener("change", (e) => {
  let saved = null;
  try { saved = localStorage.getItem("strata.theme"); } catch (err) { /* ignore */ }
  if (!saved) setTheme(e.matches ? "dark" : "light", false);
});

// ------------------------------------------------------------------ toasts
let toastId = 0;
export function toast(kind, title, text = "", ms = 3500, action = null) {
  const id = ++toastId;
  ui.toasts.push({id, kind, title, text, action});
  setTimeout(() => dismiss(id), ms);
}
export function dismiss(id) {
  const i = ui.toasts.findIndex((t) => t.id === id);
  if (i >= 0) ui.toasts.splice(i, 1);
}

// ------------------------------------------------------------------ clipboard (http on another host: no async clipboard)
export async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
  } catch (e) {
    const ta = document.createElement("textarea");
    ta.value = text; document.body.appendChild(ta); ta.select(); document.execCommand("copy"); ta.remove();
  }
  toast("success", "Copied to clipboard", "", 1800);
}

export {store};
