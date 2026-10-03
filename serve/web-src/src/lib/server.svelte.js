// Everything read from this Strata server: /health, /metrics (every second), /api/current-stream (5 times a second
// while the Monitor shows it), /mcp. Each poll replaces its object whole ($state.raw), so the views update only
// the values that changed.
import {store} from "./storage.js";
import {mergeHistory} from "./history.js";
import {ui, toast} from "./ui.svelte.js";

export const auth = $state({key: store.get("apikey", ""), needed: false});
export function setKey(key) {
  auth.key = key.trim();
  store.set("apikey", auth.key);
  auth.needed = false;
  toast("success", "API key saved", "Kept in this browser only.");
}
export function headers(json = false) {
  const h = {};
  if (auth.key) h.Authorization = "Bearer " + auth.key;
  if (json) h["Content-Type"] = "application/json";
  return h;
}

class Server {
  health = $state.raw({model: "strata", images: false, max_context: 0, web_chat: true});
  healthLoaded = $state(false);
  metrics = $state.raw(null);           // the last /metrics, with `history` replaced by the browser's longer one
  reachable = $state(true);
  stream = $state.raw({state: "idle"}); // the last /api/current-stream
  streamStatus = $state("");            // "API key needed", "Disconnected", ... when the preview cannot be read
  mcp = $state.raw({servers: [], tools: 0});
}
export const server = new Server();

class Monitor {
  live = $state(true);
  range = $state(60);
  showAll = $state(false);
  snapshot = $state.raw(null);          // what a paused Monitor keeps showing
  get view() { return this.live ? server.metrics : this.snapshot; }
  pause(paused) { this.snapshot = paused ? server.metrics : null; this.live = !paused; }
}
export const monitor = new Monitor();

// ------------------------------------------------------------------ /health
export async function loadHealth() {
  for (;;) {
    try {
      server.health = {web_chat: true, ...(await (await fetch("health")).json())};
      server.healthLoaded = true;
      return;
    } catch (e) {
      await new Promise((r) => setTimeout(r, 2000));
    }
  }
}

// ------------------------------------------------------------------ /metrics, every second
// The server retains 60 one-second readings; the browser keeps up to five minutes, across reloads too.
let history = store.get("history", null);
let savedAt = 0;
let failures = 0, keyWarned = false;
async function fetchMetrics() {
  try {
    const r = await fetch(monitor.showAll ? "metrics?requests=all" : "metrics", {headers: headers()});
    if (r.status === 401) {
      auth.needed = true;
      if (!keyWarned) { keyWarned = true; toast("warn", "API key needed", "This server needs a key: add it under Setup.", 6000); }
    } else if (r.ok) {
      const m = await r.json();
      history = mergeHistory(history, m.history, Date.now());
      m.history = history.series;
      if (Date.now() - savedAt > 5000) { store.set("history", history); savedAt = Date.now(); }
      auth.needed = false;
      failures = 0;
      server.reachable = true;
      server.metrics = m;
    } else {
      throw new Error(`HTTP ${r.status}`);
    }
  } catch (e) {
    if (++failures >= 3) server.reachable = false;
  }
}
async function pollMetrics() {
  await fetchMetrics();
  setTimeout(pollMetrics, 1000);
}
export const refreshMetrics = fetchMetrics;

// ------------------------------------------------------------------ /api/current-stream, only while it is shown
async function pollStream() {
  if (ui.tab === "monitor" && monitor.live && !document.hidden) {
    try {
      const r = await fetch("api/current-stream", {headers: headers(), cache: "no-store"});
      if (r.ok) { server.stream = await r.json(); server.streamStatus = ""; }
      else { server.stream = {state: "idle"}; server.streamStatus = r.status === 401 ? "API key needed" : "Unavailable"; }
    } catch (_) {
      server.stream = {state: "idle"}; server.streamStatus = "Disconnected";
    }
  }
  setTimeout(pollStream, 200);
}

// ------------------------------------------------------------------ /mcp (server states change rarely)
let mcpRetry = null;
export async function loadMcp() {
  try {
    const r = await fetch("mcp", {headers: headers()});
    if (!r.ok) return;
    server.mcp = await r.json();
  } catch (e) { return; /* an older server: no MCP */ }
  clearTimeout(mcpRetry);                          // right after the start, servers may still be starting (npx downloads)
  if ((server.mcp.servers || []).some((s) => s.status === "starting")) mcpRetry = setTimeout(loadMcp, 3000);
}
function pollMcp() {
  if (ui.tab === "monitor" && !document.hidden) loadMcp();
  setTimeout(pollMcp, 10000);
}

// the engine was started with the experimental-speed-projection control vector (INFO cvec=...)
export const projectionLoaded = () => {
  const c = server.metrics?.engine?.cvec;
  return !!c && c !== "0";
};

export function start() {
  addEventListener("pagehide", () => { if (history) store.set("history", history); });
  loadHealth().then(loadMcp);
  pollMetrics();
  pollStream();
  setTimeout(pollMcp, 10000);
}
