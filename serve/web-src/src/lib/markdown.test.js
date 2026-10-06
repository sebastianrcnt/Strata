import {test, expect} from "bun:test";
import {markdown} from "./markdown.js";

test("markup in the model's text is escaped", () => {
  expect(markdown("<img src=x onerror=alert(1)>")).toBe("<p>&lt;img src=x onerror=alert(1)&gt;</p>");
});
test("code blocks, also while still streaming", () => {
  expect(markdown("```py\nprint(1)\n```")).toContain("<pre><code>print(1)</code></pre>");
  expect(markdown("```py\nprint(1)")).toContain("<pre><code>print(1)</code></pre>");
});
test("lists, tables and inline styles", () => {
  expect(markdown("- a\n- **b**")).toBe("<ul><li>a</li><li><strong>b</strong></li></ul>");
  expect(markdown("| a | b |\n|---|---|\n| 1 | 2 |")).toContain("<td>1</td><td>2</td>");
});
