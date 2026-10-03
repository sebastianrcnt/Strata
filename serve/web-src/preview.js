// bun run preview: the built page (serve/web) as the Strata server would serve it, with the API passed through to
// a running server (STRATA_URL, default this PC's). For checking a build without restarting the server.
const SERVER = (process.env.STRATA_URL || "http://127.0.0.1:8081").replace(/\/$/, "");
const WEB = new URL("../web/", import.meta.url);
const port = Number(process.env.PORT || 5174);

Bun.serve({
  port,
  async fetch(req) {
    const url = new URL(req.url);
    const path = url.pathname.replace(/\/$/, "");
    if (path === "") return new Response(Bun.file(new URL("index.html", WEB)), {headers: {"Content-Type": "text/html; charset=utf-8"}});
    for (const [prefix, dir] of [["/web/", ""], ["/fonts/", "fonts/"]]) {
      const name = path.startsWith(prefix) ? path.slice(prefix.length) : null;
      if (name && !name.includes("/") && !name.includes("..")) {
        const f = Bun.file(new URL(dir + name, WEB));
        if (await f.exists()) return new Response(f, {headers: {"Cache-Control": "no-cache"}});
      }
    }
    const headers = new Headers(req.headers);
    headers.delete("host");
    return fetch(SERVER + url.pathname + url.search, {method: req.method, headers, body: req.body, redirect: "manual"});
  },
});
console.log(`preview: http://localhost:${port}/ (API: ${SERVER})`);
