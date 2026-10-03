// e2e harness: the built page (serve/web) served as the Strata server serves it, with a fake API whose state a test
// can change, and a Chrome driven by playwright-core. No GPU, model or Strata server needed.
//   E2E_BROWSER: a Playwright channel (default "chrome", the installed Google Chrome) or "chromium" for Playwright's
//   own build (bunx playwright-core install chromium).
import {chromium} from "playwright-core";
import metrics from "./fixtures/metrics.json";
import mcp from "./fixtures/mcp.json";

const WEB = new URL("../../web/", import.meta.url);

export function fakeServer() {
  const state = {
    health: {status: "ok", max_context: 262144, model: "orca-local", images: true, api_key: false, loaded: true, service: "strata", web_chat: false},
    metrics: structuredClone(metrics),
    stream: {state: "idle", request: null, phase: null, tail: "", prompt_preview: "", generated: 0, prompt_tokens: null,
             prompt_read: null, prompt_total: null, tok_s: null},
    settings: {shared: false, defaults: {}},
    mcp,
    experts: {available: false},
    polls: 0,
  };
  const json = (o) => new Response(JSON.stringify(o), {headers: {"Content-Type": "application/json"}});
  const srv = Bun.serve({
    port: 0,
    async fetch(req) {
      const path = new URL(req.url).pathname.replace(/\/$/, "");
      if (path === "") return new Response(Bun.file(new URL("index.html", WEB)), {headers: {"Content-Type": "text/html"}});
      if (path.startsWith("/web/")) {
        const f = Bun.file(new URL(path.slice(5), WEB));
        return (await f.exists()) ? new Response(f) : new Response("", {status: 404});
      }
      if (path === "/health") return json(state.health);
      if (path === "/metrics") { state.polls++; return json(state.metrics); }
      if (path === "/api/current-stream") return json(state.stream);
      if (path === "/mcp") return json(state.mcp);
      if (path === "/experts") return json(state.experts);
      if (path === "/settings") {
        if (req.method === "POST") { const b = await req.json(); state.settings = {shared: !!b.defaults, defaults: b.defaults || {}}; }
        return json(state.settings);
      }
      return new Response("", {status: 404});
    },
  });
  return {state, url: `http://localhost:${srv.port}/`, stop: () => srv.stop(true)};
}

export async function browser() {
  const channel = process.env.E2E_BROWSER ?? "chrome";
  return chromium.launch(channel === "chromium" ? {} : {channel});
}

// a page that records errors; `scheme` is the system theme it pretends to have
export async function open(b, url, {width = 1440, height = 900, scheme = "dark", hash = "", phone = false, userAgent} = {}) {
  const ctx = await b.newContext({viewport: {width, height}, colorScheme: scheme, ...(phone ? {hasTouch: true, isMobile: true} : {}),
                                  ...(userAgent ? {userAgent} : {})});
  const page = await ctx.newPage();
  const errors = [];
  page.on("pageerror", (e) => errors.push(e.message));
  page.on("console", (m) => { if (m.type() === "error") errors.push(m.text()); });
  await page.goto(url + hash);
  await page.waitForSelector(".panel, .view--chat");
  return {page, errors, ctx};
}

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// a USAGE report like the engine's: the VRAM cache holds the profile's first `slots` experts, lookups fall off with
// the rank, and those the cache does not hold go to the CPU (a few over PCIe). `scale` grows the counts.
export function usageReport(slots = 8980, scale = 1) {
  const bin = require("node:fs").readFileSync(new URL("../../../data/expert-profile.bin", import.meta.url));
  const h = new Uint32Array(bin.buffer.slice(bin.byteOffset + 4, bin.byteOffset + 24));
  const L = h[1], E = h[2], n = L * E, pairs = new Uint16Array(bin.buffer.slice(bin.byteOffset + 24, bin.byteOffset + 24 + h[4] * 4));
  const b = new Uint8Array(n * 12 + Math.ceil(n / 8)), u = new Uint32Array(b.buffer, 0, n * 3);
  for (let r = 0; r < pairs.length / 2; r++) {
    const i = pairs[2 * r] * E + pairs[2 * r + 1], c = Math.floor((scale * 2e7) / (r + 50) ** 1.5);
    if (r < slots) { u[i] = c; b[n * 12 + (i >> 3)] |= 1 << (i & 7); }
    else if (r % 5 === 0) u[n + i] = c;
    else u[2 * n + i] = c;
  }
  return Buffer.from(b).toString("base64");
}
