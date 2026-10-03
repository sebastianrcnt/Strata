// The Memory tab against a fake server: bun run e2e
import {test, expect, beforeAll, afterAll} from "bun:test";
import {fakeServer, browser, open} from "./harness.js";

let srv, b;
beforeAll(async () => { srv = fakeServer(); b = await browser(); });
afterAll(async () => { await b?.close(); srv?.stop(); });

test("the cache's share of the experts against the share of the lookups it answers", async () => {
  const {page, errors} = await open(b, srv.url, {hash: "#memory"});
  await page.waitForSelector(".layers .bar");
  const lead = await page.textContent(".lead");
  expect(lead).toContain("36.5%");                   // 8,980 expert_slots of 48 x 512 (the profile)
  expect(lead).toContain("88.9%");                   // the last request's hit rate
  expect(lead).toContain("2.4×");
  expect(await page.$$eval(".layers .bar", (b) => b.length)).toBe(48);
  expect(await page.$$eval(".chart .bar", (b) => b.length)).toBe(srv.state.metrics.requests.filter((r) => r.hit_rate != null).length);
  await page.hover(".layers .bar >> nth=28");
  expect(await page.textContent(".layers .head")).toContain("Layer 28");
  expect(await page.textContent(".tiers")).toContain("PLE n-gram table");
  expect(errors).toEqual([]);
}, 30000);
