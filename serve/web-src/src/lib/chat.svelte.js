// The Chat tab: one conversation kept in this browser, streamed from /v1/chat/completions. Tools from the server's
// MCP servers are offered per request ("strata_mcp": true, which only this page sends).
import {store} from "./storage.js";
import {fmt} from "./format.js";
import {toast} from "./ui.svelte.js";
import {server, headers, projectionLoaded} from "./server.svelte.js";

export const DEFAULTS = {thinking: "high", temperature: 0.6, top_p: 0.95, top_k: 20, max: "", seed: "", show: true, esp: true, mcp: true};

class Chat {
  settings = $state({...DEFAULTS, ...store.get("sampling", {})});
  messages = $state(store.get("chat", []));
  attachments = $state([]);            // {kind: "image" | "file", name, url | text}
  busy = $state(null);                 // {controller, msg}
}
export const chat = new Chat();

export function saveSettings(s) {
  chat.settings = {...s};
  store.set("sampling", chat.settings);
}
export function saveChat() {
  store.set("chat", chat.messages.map((m) => ({...m, images: (m.images || []).map((i) => ({name: i.name})),
                                                files: (m.files || []).map((f) => ({name: f.name}))})));
}

// ------------------------------------------------------------------ what the API gets
// a file's text in the message, fenced with more backticks than it contains itself
function fileBlock(f) {
  const longest = Math.max(2, ...(f.text.match(/`+/g) || []).map((s) => s.length));
  const fence = "`".repeat(longest + 1);
  return `File: ${f.name}\n${fence}\n${f.text}\n${fence}`;
}
function userText(m) {
  const files = (m.files || []).filter((f) => f.text != null);
  return [m.text, ...files.map(fileBlock)].filter((s) => s).join("\n\n");
}
// An answer that used MCP tools goes back as the model wrote it: per round the text before the calls, the calls and
// their results (as the model read them), then the rest - so the next question can build on what the tools found.
function assistantMessages(m) {
  const ran = (m.tools || []).filter((t) => t.round != null && t.result != null && t.state !== "skipped");
  if (!ran.length) return m.text ? [{role: "assistant", content: m.text}] : [];
  const out = [];
  let pos = 0;
  for (const r of [...new Set(ran.map((t) => t.round))]) {
    const calls = ran.filter((t) => t.round === r);
    const at = Math.min(Math.max(pos, calls[0].at || 0), m.text.length);
    out.push({role: "assistant", content: m.text.slice(pos, at).trim(),
              tool_calls: calls.map((t) => ({id: t.id, type: "function", function: {name: t.name, arguments: JSON.stringify(t.arguments || {})}}))});
    for (const t of calls) out.push({role: "tool", tool_call_id: t.id, content: t.result});
    pos = at;
  }
  const rest = m.text.slice(pos).trim();
  if (rest) out.push({role: "assistant", content: rest});
  return out;
}
function apiMessages(messages) {
  const out = [];
  for (const m of messages) {
    if (m.role === "user") {
      const imgs = (m.images || []).filter((i) => i.url);
      const text = userText(m);
      out.push({role: "user", content: imgs.length ? [{type: "text", text},
        ...imgs.map((i) => ({type: "image_url", image_url: {url: i.url}}))] : text});
    } else if (!m.error) {
      out.push(...assistantMessages(m));
    }
  }
  return out;
}

// a tool event from the stream (the `strata_mcp` field of a chunk)
function onTool(m, x, textLen, reasoningLen) {
  if (x.event === "limit") { m.limit = x.max_rounds; return; }
  m.tools = m.tools || [];
  let t = m.tools.find((y) => y.id === x.id);
  if (!t) { m.tools.push({id: x.id, name: x.name, at: textLen, rat: reasoningLen, state: "writing"}); t = m.tools[m.tools.length - 1]; }
  if (x.event === "call") {
    Object.assign(t, {name: x.name, server: x.server, tool: x.tool, arguments: x.arguments, round: x.round, state: "running"});
  } else if (x.event === "result") {
    Object.assign(t, {result: x.text, ok: x.ok, chars: x.chars, truncated: x.truncated, ms: x.ms,
                      state: x.skipped ? "skipped" : x.ok ? "done" : "error"});
  }
}

// ------------------------------------------------------------------ send
export async function send(question) {
  question = question.trim();
  if ((!question && !chat.attachments.length) || chat.busy) return false;
  chat.messages.push({role: "user", text: question, images: chat.attachments.filter((a) => a.kind !== "file"),
                      files: chat.attachments.filter((a) => a.kind === "file"), time: Date.now()});
  chat.attachments = [];
  chat.messages.push({role: "assistant", text: "", reasoning: "", time: Date.now()});
  const m = chat.messages[chat.messages.length - 1];
  const controller = new AbortController();
  chat.busy = {controller, msg: m};
  const s = chat.settings;

  const body = {model: server.health.model, messages: apiMessages(chat.messages.slice(0, -1)), stream: true,
                reasoning_effort: s.thinking};
  if (s.temperature > 0) Object.assign(body, {temperature: +s.temperature, top_p: +s.top_p, top_k: +s.top_k});
  else body.temperature = 0;
  if (s.seed) body.seed = +s.seed;
  if (s.max) body.max_tokens = +s.max;
  if (projectionLoaded()) body.experimental_speed_projection = !!s.esp;
  if (s.mcp !== false && server.mcp.tools > 0) body.strata_mcp = true;   // this server may run MCP tools for it

  // the stream is gathered here and handed to the view once per frame
  let text = "", reasoning = "", firstAt = null, thinkStart = null, usage = null, frame = 0;
  const paint = () => { frame = 0; m.text = text; m.reasoning = reasoning; };
  try {
    const r = await fetch("v1/chat/completions", {method: "POST", headers: headers(true), body: JSON.stringify(body),
                                                   signal: controller.signal});
    if (!r.ok) {
      let msg = `HTTP ${r.status}`;
      try { msg = (await r.json()).error.message || msg; } catch (e) { /* not json */ }
      if (r.status === 401) msg = "This server needs an API key: add it under Setup.";
      throw new Error(msg);
    }
    const reader = r.body.getReader(), dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const {value, done} = await reader.read();
      if (done) break;
      buf += dec.decode(value, {stream: true});
      let nl;
      while ((nl = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, nl).trim();
        buf = buf.slice(nl + 1);
        if (!line.startsWith("data:")) continue;              // ": keep-alive" comments while a long prompt is read
        const data = line.slice(5).trim();
        if (data === "[DONE]") continue;
        let j;
        try { j = JSON.parse(data); } catch (e) { continue; }
        if (j.error) throw new Error(j.error.message || "the engine reported an error");
        if (j.usage) usage = j.usage;
        if (j.strata_mcp) { paint(); onTool(m, j.strata_mcp, text.length, reasoning.length); }
        const d = (j.choices && j.choices[0] && j.choices[0].delta) || {};
        const lastTool = m.tools && m.tools.length ? m.tools[m.tools.length - 1] : null;   // a new round after a tool
        if (d.reasoning_content) {
          if (!firstAt) firstAt = performance.now();
          if (!thinkStart) thinkStart = performance.now();
          if (lastTool && reasoning && lastTool.rat === reasoning.length) reasoning += "\n\n";
          reasoning += d.reasoning_content;
        }
        if (d.content) {
          if (!firstAt) firstAt = performance.now();
          if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
          if (lastTool && text && lastTool.at === text.length) text += "\n\n";
          text += d.content;
        }
        if (!frame) frame = requestAnimationFrame(paint);
      }
    }
  } catch (e) {
    if (e.name === "AbortError") m.stopped = true;
    else { m.error = e.message || String(e); toast("error", "The request failed", m.error, 6000); }
  }
  if (frame) cancelAnimationFrame(frame);
  paint();
  if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
  const n = usage ? usage.completion_tokens : null;
  let meta = "";
  if (n && firstAt) {
    const secs = (performance.now() - firstAt) / 1000;
    meta = `${fmt(n)} tokens${secs > 0.25 ? ` · ${fmt(n / secs, 1)} tok/s` : ""}${m.stopped ? " · stopped" : ""}` +
           (projectionLoaded() ? (s.esp ? " · projection on" : " · projection off") : "");
  } else if (m.stopped) {
    meta = "Stopped";
  }
  for (const t of m.tools || []) if (t.state === "writing" || t.state === "running") { t.state = "skipped"; t.ms = null; }
  const ran = (m.tools || []).filter((t) => t.state === "done" || t.state === "error").length;
  if (ran) meta = `${meta ? `${meta} · ` : ""}${ran} tool call${ran > 1 ? "s" : ""}`;
  if (m.limit) meta = `${meta} · stopped at the limit of ${m.limit} tool rounds (mcp.max_rounds)`;
  m.meta = meta;
  chat.busy = null;
  saveChat();
  return true;
}
export const stop = () => chat.busy?.controller.abort();

export function newChat() {
  if (chat.busy) { toast("warn", "Still writing", "Stop the answer first."); return; }
  if (!chat.messages.length) return;
  const backup = $state.snapshot(chat.messages);
  chat.messages = [];
  saveChat();
  toast("info", "New chat", "The last one was cleared.", 6000, {label: "Undo", run: () => { chat.messages = backup; saveChat(); }});
}

export function exportChat() {
  const messages = chat.messages;
  if (!messages.length) { toast("info", "Nothing to save yet"); return; }
  const tools = (m) => (m.tools || []).filter((t) => t.result != null).map((t) =>
    `<details><summary>Tool ${t.server ? `${t.server} / ` : ""}${t.tool || t.name}${t.ok ? "" : " (error)"}</summary>\n\n` +
    `\`\`\`json\n${JSON.stringify(t.arguments || {}, null, 2)}\n\`\`\`\n\n\`\`\`\n${t.result}\n\`\`\`\n\n</details>\n\n`).join("");
  const md = messages.map((m) => m.role === "user" ? `## You\n\n${m.text}\n` :
    `## ${server.health.model}\n\n${m.reasoning ? `<details><summary>Thinking</summary>\n\n${m.reasoning}\n\n</details>\n\n` : ""}${tools(m)}${m.text || m.error || ""}\n`).join("\n");
  const a = document.createElement("a");
  a.href = URL.createObjectURL(new Blob([md], {type: "text/markdown"}));
  a.download = `strata-chat-${new Date().toISOString().slice(0, 16).replace(/[:T]/g, "-")}.md`;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

// ------------------------------------------------------------------ attachments: pictures and text files (issue #30)
const TEXT_EXT = /\.(txt|md|markdown|rst|tex|py|pyi|ipynb|js|mjs|cjs|ts|tsx|jsx|vue|svelte|json|jsonl|csv|tsv|log|ya?ml|toml|ini|cfg|conf|env|xml|html?|css|scss|less|c|cc|cpp|cxx|h|hh|hpp|cu|cuh|rs|go|java|kt|kts|swift|rb|php|pl|lua|r|jl|scala|sql|sh|bash|zsh|fish|ps1|psm1|bat|cmd|diff|patch|gradle|cmake|mk|dockerfile|gitignore|proto|graphql)$/i;
const MAX_TEXT_FILE = 512 * 1024;
function isTextFile(f) {
  return f.type.startsWith("text/") || /json|xml|javascript|yaml|toml|x-sh|x-python/.test(f.type) ||
         TEXT_EXT.test(f.name) || /(^|[\\/])(makefile|dockerfile|readme|license)$/i.test(f.name);
}
export function addFiles(files) {
  const images = server.health.images;
  for (const f of files) {
    if (f.type.startsWith("image/")) {
      if (!images) { toast("warn", "Pictures are off", "This model was set up for text only."); continue; }
      if (f.size > 20e6) { toast("warn", "Picture too large", `${f.name} is over 20 MB.`); continue; }
      const r = new FileReader();
      r.onload = () => chat.attachments.push({kind: "image", name: f.name || "pasted image", url: r.result});
      r.readAsDataURL(f);
      continue;
    }
    if (!isTextFile(f)) { toast("warn", "Not a text file", `${f.name}: attach text files (code, notes, logs, data)${images ? " or pictures" : ""}.`); continue; }
    if (f.size > MAX_TEXT_FILE) { toast("warn", "File too large", `${f.name} is over 512 KB.`); continue; }
    const r = new FileReader();
    r.onload = () => {
      const text = String(r.result);
      if (text.includes("\u0000")) { toast("warn", "Not a text file", `${f.name} looks like a binary file.`); return; }
      chat.attachments.push({kind: "file", name: f.name, text});
    };
    r.readAsText(f);
  }
}
