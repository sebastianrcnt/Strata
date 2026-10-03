// The Memory tab against a fake server: bun run e2e
import {test, expect, beforeAll, afterAll} from "bun:test";
import {fakeServer, browser, open, sleep} from "./harness.js";

let srv, b;
beforeAll(async () => { srv = fakeServer(); b = await browser(); });
afterAll(async () => { await b?.close(); srv?.stop(); });

// the share of the map's pixels that are lit (not the background)
const lit = (page) => page.$eval("canvas", (c) => {
  const d = c.getContext("2d").getImageData(0, 0, c.width, c.height).data;
  let n = 0;
  for (let i = 0; i < d.length; i += 4 * 7) if (d[i] + d[i + 1] + d[i + 2] > 200) n++;
  return n / (d.length / (4 * 7));
});

test("the tiers and the map come from the engine's numbers and the expert profile", async () => {
  const {page, errors} = await open(b, srv.url, {hash: "#memory"});
  await page.waitForSelector("canvas");
  const text = await page.textContent(".memory");
  expect(text).toContain("24,576");                  // 48 x 512 from the profile
  expect(text).toContain("8,980");                   // the engine's expert_slots
  expect(text).toContain("VRAM cache 88.9%");        // the last request's hit rate
  await sleep(2000);                                  // the cache fills in rank order when the tab opens
  expect(await lit(page)).toBeGreaterThan(.1);
  const c = await (await page.$("canvas")).boundingBox();
  const at = (l, col) => page.mouse.move(c.x + (col + .5) * c.width / 512, c.y + (l + .5) * c.height / 48);
  await at(0, 0);
  expect(await page.textContent(".tip")).toContain("Layer 0 · expert 0");
  await page.click("text=By rank");
  await at(28, 0);                                    // the profile's first pair: layer 28, expert 288
  expect(await page.textContent(".tip")).toContain("Layer 28 · expert 288 rank 1 of 24,576");
  expect(errors).toEqual([]);
}, 30000);

test("generating: the sweep lights experts and the rates show", async () => {
  srv.state.metrics.live = {...srv.state.metrics.live, state: "generating", tok_s: 48};
  try {
    const {page, errors} = await open(b, srv.url, {hash: "#memory"});
    await page.waitForSelector("canvas");
    await sleep(2500);
    expect(await page.textContent(".readouts")).toContain("≈ 23,040");   // 48 tok/s x 48 layers x 10
    expect(await page.$$eval(".legend span", (s) => s.map((x) => x.textContent))).toContain("computed by the CPU");
    expect(errors).toEqual([]);
  } finally {
    srv.state.metrics.live = {...srv.state.metrics.live, state: "idle"};
  }
}, 30000);
