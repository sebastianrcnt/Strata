// The Chat tab: one conversation kept in this browser, streamed from /v1/chat/completions. Pictures go inline as
// data: URLs (the server takes no other kind).
import {store} from "./storage.js";
import {fmt} from "./format.js";
import {toast} from "./ui.svelte.js";
import {server, headers, projectionLoaded} from "./server.svelte.js";

export const DEFAULTS = {thinking: "high", temperature: 0.6, top_p: 0.95, top_k: 20, max: "", seed: "", show: true, esp: true};

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
function apiMessages(messages) {
  const out = [];
  for (const m of messages) {
    if (m.role === "user") {
      const imgs = (m.images || []).filter((i) => i.url?.startsWith("data:"));
      const text = userText(m);
      out.push({role: "user", content: imgs.length ? [{type: "text", text},
        ...imgs.map((i) => ({type: "image_url", image_url: {url: i.url}}))] : text});
    } else if (!m.error && m.text) {
      out.push({role: "assistant", content: m.text});
    }
  }
  return out;
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

  // the stream is gathered here and handed to the view once per frame
  // m.live while it streams: the tokens so far (one per streamed piece) and the rate over the last two seconds
  const sentAt = performance.now();
  let text = "", reasoning = "", firstAt = null, thinkStart = null, usage = null, frame = 0, pieces = 0;
  const recent = [];
  const paint = () => {
    frame = 0; m.text = text; m.reasoning = reasoning;
    if (!pieces) return;
    const now = performance.now();
    while (recent.length > 1 && now - recent[0] > 2000) recent.shift();
    const span = (now - recent[0]) / 1000;
    m.live = {tokens: pieces, tok_s: recent.length > 1 && span > .25 ? (recent.length - 1) / span : null};
  };
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
        const d = (j.choices && j.choices[0] && j.choices[0].delta) || {};
        if (d.reasoning_content || d.content) { pieces++; recent.push(performance.now()); }
        if (d.reasoning_content) {
          if (!firstAt) firstAt = performance.now();
          if (!thinkStart) thinkStart = performance.now();
          reasoning += d.reasoning_content;
        }
        if (d.content) {
          if (!firstAt) firstAt = performance.now();
          if (thinkStart && m.thinkSecs == null) m.thinkSecs = (performance.now() - thinkStart) / 1000;
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
    meta = `${fmt(n)} tokens${secs > 0.25 ? ` · ${fmt(n / secs, 1)} tok/s` : ""} · first token after ${fmt((firstAt - sentAt) / 1000, 1)} s` +
           `${m.stopped ? " · stopped" : ""}` +
           (projectionLoaded() ? (s.esp ? " · projection on" : " · projection off") : "");
  } else if (m.stopped) {
    meta = "Stopped";
  }
  m.meta = meta;
  m.live = null;
  chat.busy = null;
  saveChat();
  return true;
}
export const stop = () => chat.busy?.controller.abort();

// the last question again, for a new answer (its pictures only while this page still holds them)
export function retry() {
  if (chat.busy) return;
  const i = chat.messages.findLastIndex((m) => m.role === "user");
  if (i < 0) return;
  const q = chat.messages[i];
  const attachments = [...(q.images || []).filter((x) => x.url?.startsWith("data:")).map((x) => ({kind: "image", ...x})),
                       ...(q.files || []).filter((f) => f.text != null).map((f) => ({kind: "file", ...f}))];
  if (!q.text?.trim() && !attachments.length) return;
  chat.messages = chat.messages.slice(0, i);
  chat.attachments = attachments;
  send(q.text || "");
}

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
  const md = messages.map((m) => m.role === "user" ? `## You\n\n${m.text}\n` :
    `## ${server.health.model}\n\n${m.reasoning ? `<details><summary>Thinking</summary>\n\n${m.reasoning}\n\n</details>\n\n` : ""}${m.text || m.error || ""}\n`).join("\n");
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
