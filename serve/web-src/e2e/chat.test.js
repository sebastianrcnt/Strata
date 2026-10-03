// The Chat tab (web_chat on) with a scripted stream instead of a model: bun run e2e
import {test, expect, beforeAll, afterAll} from "bun:test";
import {fakeServer, browser, open, sleep} from "./harness.js";

let srv, b;
beforeAll(async () => { srv = fakeServer(); srv.state.health.web_chat = true; b = await browser(); });
afterAll(async () => { await b?.close(); srv?.stop(); });

const chunk = (o) => `data: ${JSON.stringify(o)}\n\n`;
const delta = (d) => chunk({choices: [{delta: d}]});
const STREAM = [
  ": keep-alive\n\n",
  delta({reasoning_content: "Let me look. "}),
  chunk({strata_mcp: {event: "call", id: "c1", name: "host__bash", server: "host", tool: "bash", arguments: {command: "ls"}, round: 1}}),
  chunk({strata_mcp: {event: "result", id: "c1", text: "a.txt\nb.txt", ok: true, chars: 11, ms: 420}}),
  delta({content: "Files:\n\n- `a.txt`\n\n<script>alert(1)</script>"}),
  chunk({choices: [{delta: {}}], usage: {completion_tokens: 42}}),
  "data: [DONE]\n\n",
].join("");

async function chatPage() {
  const o = await open(b, srv.url, {width: 1280});
  o.sent = [];
  await o.page.route("**/v1/chat/completions", async (r) => {
    o.sent.push(JSON.parse(r.request().postData()));
    await r.fulfill({status: 200, headers: {"Content-Type": "text/event-stream"}, body: STREAM});
  });
  await o.page.evaluate(() => localStorage.removeItem("strata.chat"));
  return o;
}

test("a streamed answer: thinking, a tool block, escaped Markdown, the tool round goes back to the API", async () => {
  const {page, sent, errors} = await chatPage();
  await page.fill("textarea", "list the files");
  await page.keyboard.press("Enter");
  await page.waitForSelector(".msg--assistant .meta >> text=42 tokens");
  expect(await page.$(".answer script")).toBeNull();
  expect(await page.textContent(".answer")).toContain("<script>alert(1)</script>");
  expect(await page.textContent(".tool summary")).toContain("bash");
  await page.fill("textarea", "thanks");
  await page.keyboard.press("Enter");
  await page.waitForFunction(() => document.querySelectorAll(".msg--assistant .meta").length === 2);
  expect(sent[1].messages.map((m) => m.role + (m.tool_calls ? "+calls" : ""))).toEqual(["user", "assistant+calls", "tool", "assistant", "user"]);
  expect(sent[0].strata_mcp).toBeUndefined();   // tools are off until the chat's settings turn them on
  expect(await page.textContent(".msg--assistant .meta >> nth=0")).toMatch(/first token after [\d.]+ s/);
  expect(errors).toEqual([]);
}, 30000);

test("new chat can be undone; the chat survives a reload", async () => {
  const {page} = await chatPage();
  await page.fill("textarea", "hi");
  await page.keyboard.press("Enter");
  await page.waitForSelector(".msg--assistant .meta >> text=42 tokens");
  await page.reload();
  await page.waitForSelector(".msg");
  expect(await page.$$eval(".msg", (x) => x.length)).toBe(2);
  await page.click(".top button:has-text('New chat')");
  expect(await page.$$eval(".msg", (x) => x.length)).toBe(0);
  await page.click(".toast-action");
  expect(await page.$$eval(".msg", (x) => x.length)).toBe(2);
}, 30000);

test("the settings sheet keeps focus inside and gives it back when closed", async () => {
  const {page} = await chatPage();
  await page.click("button[aria-label='Chat settings']");
  await page.waitForSelector(".sheet");
  for (let i = 0; i < 25; i++) await page.keyboard.press("Tab");
  expect(await page.evaluate(() => !!document.activeElement.closest(".sheet"))).toBe(true);
  await page.keyboard.press("Escape");
  await sleep(300);
  expect(await page.evaluate(() => document.activeElement.getAttribute("aria-label"))).toBe("Chat settings");
}, 30000);

test("stop ends a slow answer", async () => {
  const {page} = await chatPage();
  await page.unroute("**/v1/chat/completions");
  await page.route("**/v1/chat/completions", async (r) => { await sleep(5000); await r.fulfill({status: 200, body: ""}).catch(() => {}); });
  await page.fill("textarea", "slow");
  await page.keyboard.press("Enter");
  await page.click("text=Stop");
  await page.waitForSelector(".msg--assistant .meta >> text=Stopped");
}, 30000);

test("answer again: the last question goes back for a new answer, the old one is replaced", async () => {
  const {page, sent} = await chatPage();
  await page.fill("textarea", "hi");
  await page.keyboard.press("Enter");
  await page.waitForSelector(".msg--assistant .meta >> text=42 tokens");
  expect(await page.$$eval("button[aria-label='Answer again']", (x) => x.length)).toBe(1);
  await page.click("button[aria-label='Answer again']");
  await page.waitForFunction(() => document.querySelector(".msg--assistant .meta")?.textContent.includes("42 tokens"));
  expect(await page.$$eval(".msg", (x) => x.length)).toBe(2);
  expect(sent.length).toBe(2);
  expect(sent[1].messages.map((m) => m.role)).toEqual(["user"]);
}, 30000);

test("while it streams: the tokens and the rate so far; before the first one, what the engine is busy with", async () => {
  const {page} = await chatPage();
  await page.unroute("**/v1/chat/completions");
  srv.state.metrics = {...srv.state.metrics, live: {...srv.state.metrics.live, state: "generating", queued: 1}};
  let release;
  const gate = new Promise((r) => (release = r));
  await page.route("**/v1/chat/completions", async (r) => { await gate; await r.fulfill({status: 200, headers: {"Content-Type": "text/event-stream"}, body: STREAM}); });
  await page.fill("textarea", "wait");
  await page.keyboard.press("Enter");
  await page.waitForSelector(".answer >> text=Waiting for another request to finish", {timeout: 4000});
  srv.state.metrics = {...srv.state.metrics, live: {...srv.state.metrics.live, state: "idle", queued: 0}};
  release();
  await page.waitForSelector(".msg--assistant .meta >> text=42 tokens");
}, 30000);

test("a phone: Enter is a new line, the button sends, the keyboard does not come up by itself, 16px input", async () => {
  const {page} = await open(b, srv.url, {width: 390, height: 760, phone: true});
  await page.route("**/v1/chat/completions", (r) => r.fulfill({status: 200, headers: {"Content-Type": "text/event-stream"}, body: STREAM}));
  expect(await page.evaluate(() => document.activeElement.tagName)).not.toBe("TEXTAREA");
  expect(await page.$eval("textarea", (t) => getComputedStyle(t).fontSize)).toBe("16px");
  await page.tap("textarea");
  await page.keyboard.type("two");
  await page.keyboard.press("Enter");
  await page.keyboard.type("lines");
  expect(await page.$$eval(".msg", (x) => x.length)).toBe(0);
  expect(await page.inputValue("textarea")).toBe("two\nlines");
  await page.tap("button[aria-label='Send']");
  await page.waitForSelector(".msg--assistant .meta >> text=42 tokens");
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
}, 30000);

test("web_chat false: Setup can show Chat on this device only", async () => {
  srv.state.health.web_chat = false;
  try {
    const {page} = await open(b, srv.url, {hash: "#about"});
    expect(await page.$$eval("[role=tab]", (t) => t.map((x) => x.textContent))).not.toContain("Chat");
    await page.click("label[for=chat-here]");
    await page.waitForSelector("[role=tab] >> text=Chat");
    await page.reload();
    await page.waitForSelector("[role=tab] >> text=Chat");
  } finally { srv.state.health.web_chat = true; }
}, 30000);
