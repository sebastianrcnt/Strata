// The Monitor and Setup tabs against a fake server: bun run e2e
import {test, expect, beforeAll, afterAll} from "bun:test";
import {fakeServer, browser, open, sleep} from "./harness.js";

let srv, b;
beforeAll(async () => { srv = fakeServer(); b = await browser(); });
afterAll(async () => { await b?.close(); srv?.stop(); });

test("web_chat false: no Chat tab, #chat lands on the Monitor, arrow keys move between tabs", async () => {
  const {page, errors} = await open(b, srv.url, {hash: "#chat"});
  expect(await page.$$eval("[role=tab]", (t) => t.map((x) => x.textContent))).toEqual(["Monitor", "Memory", "Setup"]);
  expect(await page.$eval("[role=tab][data-state=active]", (t) => t.textContent)).toBe("Monitor");
  await page.focus("#tab-btn-monitor");
  await page.keyboard.press("ArrowRight");
  expect(await page.$eval("[role=tab][data-state=active]", (t) => t.textContent)).toBe("Memory");
  expect(errors).toEqual([]);
}, 30000);

test("the request table updates in place: a selection and an open row survive the refresh", async () => {
  const {page} = await open(b, srv.url);
  await page.click("tr.row");
  await page.evaluate(() => {
    const r = document.createRange(); r.selectNodeContents(document.querySelector("tr.row td"));
    getSelection().removeAllRanges(); getSelection().addRange(r);
  });
  const polls = srv.state.polls;
  await sleep(2500);
  expect(srv.state.polls).toBeGreaterThan(polls);
  expect((await page.evaluate(() => getSelection().toString())).length).toBeGreaterThan(0);
  expect(await page.textContent("tr.details")).toContain("MTP drafts accepted");
}, 30000);

test("only exceptions get a badge", async () => {
  const {page} = await open(b, srv.url);
  const badges = await page.$$eval("tr.row .badge", (x) => x.map((e) => e.textContent));
  expect(badges).toContain("Error");
  expect(badges).toContain("Stopped");
  expect(badges).not.toContain("Done");
}, 30000);

test("the range select works from the keyboard and relabels the time ticks", async () => {
  const {page} = await open(b, srv.url);
  await page.click(".select");
  await page.keyboard.press("ArrowDown");
  await page.keyboard.press("Enter");
  await sleep(200);
  expect(await page.textContent(".select")).toContain("5 min");
  expect(await page.textContent(".ruler")).toContain("−5:00");
}, 30000);

test("a track's tooltip opens without folding it; folding is remembered", async () => {
  const {page} = await open(b, srv.url);
  const openBefore = await page.$$eval(".fold[aria-expanded=true]", (x) => x.length);
  await page.hover(".label .tip__trigger");
  await page.waitForSelector(".tip", {timeout: 3000});
  await page.click(".label .tip__trigger");
  expect(await page.$$eval(".fold[aria-expanded=true]", (x) => x.length)).toBe(openBefore);
  await page.click(".fold >> nth=0");
  await page.reload();
  await page.waitForSelector(".panel");
  expect(await page.$$eval(".fold[aria-expanded=true]", (x) => x.length)).toBe(openBefore - 1);
}, 30000);

test("now: idle is one line; generating shows the input bar, the tape and the speed", async () => {
  const {page} = await open(b, srv.url);
  expect(await page.$$eval(".row__label", (x) => x.length)).toBe(0);
  srv.state.stream = {...srv.state.stream, state: "generating", request: 1, phase: "thinking", tail: "hello world", generated: 12, tok_s: 51.5};
  await page.waitForSelector(".row__label", {timeout: 3000});
  await sleep(600);
  expect(await page.textContent(".panel h2 >> nth=0")).toBe("Thinking");
  expect(await page.textContent(".tape")).toContain("hello world");
  expect(await page.textContent(".bar__status, header.bar")).toContain("51.5");
  srv.state.stream = {...srv.state.stream, state: "idle", request: null, tail: ""};
  await sleep(600);
}, 30000);

test("the graph record is kept in the browser for the next load", async () => {
  const {page} = await open(b, srv.url);
  await sleep(1500);
  await page.evaluate(() => dispatchEvent(new Event("pagehide")));
  const h = await page.evaluate(() => JSON.parse(localStorage.getItem("strata.history")));
  expect(h.series.tok_s.length).toBeGreaterThanOrEqual(60);
}, 30000);

test("phones: no sideways scroll; the requests come right after 'now'", async () => {
  const {page} = await open(b, srv.url, {width: 390, height: 844});
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  const titles = await page.$$eval(".panel h2", (x) => x.map((e) => [e.textContent, e.getBoundingClientRect().top]).sort((a, c) => a[1] - c[1]).map((e) => e[0]));
  expect(titles.slice(1, 3)).toEqual(["Recent requests", "Speed and hardware"]);
}, 30000);

test("theme: the system's until chosen; Setup's box switches it", async () => {
  const {page} = await open(b, srv.url, {scheme: "light", hash: "#about"});
  expect(await page.evaluate(() => document.documentElement.dataset.theme)).toBe("light");
  await page.click("#dark-theme");
  expect(await page.evaluate(() => document.documentElement.dataset.theme)).toBe("dark");
}, 30000);

test("defaults for other apps: off until set, then posted to the server", async () => {
  const {page} = await open(b, srv.url, {hash: "#about"});
  await page.click("text=Set defaults…");
  await page.click("text=Apply to every app");
  await sleep(300);
  expect(srv.state.settings.shared).toBe(true);
  expect(srv.state.settings.defaults.reasoning_effort).toBe("high");
  srv.state.settings = {shared: false, defaults: {}};
}, 30000);
