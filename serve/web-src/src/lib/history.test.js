import {test, expect} from "bun:test";
import {mergeHistory, idleAsGap} from "./history.js";

const s = (n, from = 1) => Array.from({length: n}, (_, i) => from + i);

test("the first poll takes the server's samples", () => {
  const h = mergeHistory(null, {a: s(60)}, 1000);
  expect(h.series.a.length).toBe(60);
  expect(h.t).toBe(1000);
});
test("a poll one second later adds one sample", () => {
  const h = mergeHistory({t: 0, series: {a: s(60)}}, {a: s(60, 2)}, 1000);
  expect(h.series.a.slice(-2)).toEqual([60, 61]);
  expect(h.series.a.length).toBe(61);
});
test("a slow poll (background tab) adds every second it missed, not one", () => {
  const h = mergeHistory({t: 0, series: {a: s(60)}}, {a: s(60, 31)}, 30000);
  expect(h.series.a.length).toBe(90);
  expect(h.series.a.at(-1)).toBe(90);
});
test("a reload after longer than the server keeps leaves a gap of nulls", () => {
  const h = mergeHistory({t: 0, series: {a: s(10)}}, {a: s(60)}, 100000);
  expect(h.series.a.length).toBe(10 + 40 + 60);
  expect(h.series.a.slice(10, 50).every((v) => v === null)).toBe(true);
});
test("a record older than the window starts over, and the record is capped", () => {
  expect(mergeHistory({t: 0, series: {a: s(300)}}, {a: s(60)}, 400000).series.a.length).toBe(60);
  expect(mergeHistory({t: 0, series: {a: s(300)}}, {a: s(60, 2)}, 1000).series.a.length).toBe(300);
});
test("idle zeros become gaps", () => {
  expect(idleAsGap([0, 5, 0, null])).toEqual([null, 5, null, null]);
});
