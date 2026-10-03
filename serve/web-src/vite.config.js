// Builds the web app into serve/web: index.html, app.js and app.css with fixed names, next to the static design
// system files (tokens.css, components.css, sprite.svg, fonts) that the server already serves as they are.
import {defineConfig} from "vite";
import {svelte} from "@sveltejs/vite-plugin-svelte";

// `bun run dev` proxies the API to a running Strata server (default: this PC's)
const SERVER = process.env.STRATA_URL || "http://127.0.0.1:8081";
const API = ["/health", "/metrics", "/api", "/mcp", "/settings", "/v1", "/web", "/fonts"];

export default defineConfig({
  plugins: [
    svelte(),
    {
      // the page is served at "/" and its files at "/web/...": relative, so it also works behind a path prefix
      name: "strata-web-paths",
      apply: "build",
      transformIndexHtml: {order: "post", handler: (html) => html.replaceAll('"./app.', '"web/app.')},
    },
  ],
  base: "./",
  build: {
    outDir: "../web",
    emptyOutDir: false,          // serve/web also holds the design system files and the API monitor page
    assetsDir: "",
    modulePreload: false,
    target: "es2022",
    rollupOptions: {
      output: {
        entryFileNames: "app.js",
        assetFileNames: (a) => (a.names?.[0] || a.name || "").endsWith(".css") ? "app.css" : "[name][extname]",
      },
    },
  },
  server: {proxy: Object.fromEntries(API.map((p) => [p, {target: SERVER, changeOrigin: true}]))},
});
