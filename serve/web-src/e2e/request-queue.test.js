import {test, expect, beforeAll, afterAll} from "bun:test";
import {fakeServer, browser, open} from "./harness.js";

let srv, b;
beforeAll(async () => { srv = fakeServer(); b = await browser(); });
afterAll(async () => { await b?.close(); srv?.stop(); });

test("Recent requests shows live work and queue counts, then the completed request", async () => {
  const original = structuredClone(srv.state.metrics);
  const {page, errors, ctx} = await open(b, srv.url);
  try {
    await page.click("tr.row");
    srv.state.metrics.live = {state: "reading", queued: 2, prompt_tokens: 1200,
      prompt_read: 200, prompt_total: 1200, generated: 0, elapsed_s: 1.5, max_tokens: 4096};
    await page.waitForSelector(".request-running");
    expect(await page.textContent(".request-running")).toContain("Reading input");
    expect(await page.textContent(".request-queued")).toContain("2 queued");
    expect(await page.textContent(".request-running td:nth-child(3)")).toContain("200 / 1,200");
    expect(await page.locator("tr.details").count()).toBe(1);

    srv.state.metrics.live = {...srv.state.metrics.live, state: "generating", phase: "thinking",
      generated: 123, elapsed_s: 4.5, tok_s: 42};
    await page.waitForFunction(() => document.querySelector(".request-running")?.textContent.includes("Thinking"));
    expect(await page.textContent(".request-running")).toContain("of 4,096 max");
    srv.state.metrics.live = {...srv.state.metrics.live, phase: "answering", queued: 1};
    await page.waitForFunction(() => document.querySelector(".request-running")?.textContent.includes("Answering"));
    expect(await page.textContent(".request-queued")).toContain("1 queued");

    srv.state.metrics.live = {state: "idle", queued: 0};
    srv.state.metrics.requests.unshift({...original.requests[0], time: Date.now() / 1000, output_tokens: 123});
    await page.waitForSelector(".request-running", {state: "detached"});
    expect(await page.locator(".request-queued").count()).toBe(0);
    expect(await page.textContent("tr.row td:nth-child(4)")).toContain("123");
    expect(await page.locator("tr.details").count()).toBe(1);
    expect(errors).toEqual([]);
  } finally { srv.state.metrics = original; await ctx.close(); }
}, 30000);

test("queue-only state with no history fits a phone and clears when idle", async () => {
  const original = structuredClone(srv.state.metrics);
  srv.state.metrics.requests = [];
  srv.state.metrics.requests_kept = 0;
  srv.state.metrics.live = {state: "idle", queued: 3};
  const {page, errors, ctx} = await open(b, srv.url, {width: 390, height: 844});
  try {
    await page.waitForSelector(".request-queued");
    expect(await page.locator(".request-running").count()).toBe(0);
    expect(await page.textContent(".slot--requests")).toContain("No completed requests yet");
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
    await page.screenshot({path: "/tmp/strata-request-queue-phone.png", fullPage: true});
    srv.state.metrics.live.queued = 0;
    await page.waitForSelector(".request-queued", {state: "detached"});
    expect(await page.textContent(".slot--requests")).toContain("No requests yet");
    expect(errors).toEqual([]);
  } finally { srv.state.metrics = original; await ctx.close(); }
}, 30000);
