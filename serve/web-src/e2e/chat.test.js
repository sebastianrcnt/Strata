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
  expect(sent[0].strata_mcp).toBe(true);
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
  await page.click("button[aria-label='New chat']");
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
