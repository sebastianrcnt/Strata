# Strata web app (source)

The page at `/` (Chat, Monitor, Setup), written in Svelte 5 and built with Vite and Bun. The build writes
`serve/web/index.html`, `serve/web/app.js` and `serve/web/app.css`; those built files are committed, so running Strata
needs neither Bun nor Node. The page's look is its own: `src/styles/tokens.css` (greys, two accents - orange for running, cyan for values and
selection - three type sizes, one radius) and the small kit in `src/ui/` (Panel, Button, Badge, Value, Check, Segmented,
Select, Tip, Sheet, Facts). Keyboard and focus behaviour of tabs, selects, toggles, tooltips and the sheet come from
[Bits UI](https://bits-ui.com) (headless); the styling is ours. `sprite.svg` (icons) is served as it is;
`tokens.css` / `components.css` in `serve/web` remain only for the API request monitor page. `monitor.html` / `monitor.js` (the API request monitor
at `/api-monitor`) are separate and unchanged.

```sh
cd serve/web-src
bun install
bun run dev        # Vite dev server with hot reload; the API goes to STRATA_URL (default http://127.0.0.1:8081)
bun run build      # writes serve/web/{index.html,app.js,app.css}: commit them with the source change
bun run preview    # the built page as the server serves it, API passed to STRATA_URL (port 5174)
bun test           # unit tests (input metrics, Markdown)
bun run check      # svelte-check
```

`"web_chat": false` in the server config (or `--no-web-chat`) hides the Chat tab; the page reads it from `/health`.
