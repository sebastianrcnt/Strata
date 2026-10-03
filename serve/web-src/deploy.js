// bun run deploy: hot-swaps the built web app into a running Strata (no restart: the server reads serve/web on every
// request, so this needs serve/web mounted or installed as a directory). Builds, runs the unit tests, backs up the
// target's web directory next to it, copies serve/web over with rsync, then checks that the server now serves this
// build's app.js.
//   STRATA_DEPLOY  rsync target of the server's serve/web, e.g. linux:/home/me/strata/deploy/docker/serve/web
//   STRATA_URL     the server, e.g. http://192.168.0.235:8081 (to check what it serves)
//   --dry-run      show what would change, copy nothing
import {$} from "bun";

const target = process.env.STRATA_DEPLOY, url = (process.env.STRATA_URL || "").replace(/\/$/, "");
const dry = process.argv.includes("--dry-run");
if (!target || !url) {
  console.error("set STRATA_DEPLOY (rsync target of serve/web) and STRATA_URL (the server)");
  process.exit(2);
}
const WEB = new URL("../web/", import.meta.url).pathname;
const sha = async (buf) => new Bun.CryptoHasher("sha256").update(buf).digest("hex");

await $`bunx vite build`;
await $`bun test src`;
const dirty = (await $`git status --porcelain -- ${WEB}`.text()).trim();
if (dirty) console.warn(`note: the built files differ from the last commit - commit them with the source:\n${dirty}`);

const [host, dir] = target.includes(":") ? [target.slice(0, target.indexOf(":")), target.slice(target.indexOf(":") + 1)] : [null, target];
const backup = `${dir.replace(/\/$/, "")}.bak-${new Date().toISOString().replace(/[-:]/g, "").slice(0, 15)}`;
if (dry) {
  await $`rsync -a --delete --dry-run --itemize-changes ${WEB} ${target.replace(/\/?$/, "/")}`;
  process.exit(0);
}
if (host) await $`ssh -o BatchMode=yes ${host} cp -a ${dir} ${backup}`;
else await $`cp -a ${dir} ${backup}`;
console.log(`backup: ${host ? `${host}:` : ""}${backup}`);
await $`rsync -a --delete ${WEB} ${target.replace(/\/?$/, "/")}`;

const local = await sha(await Bun.file(WEB + "app.js").arrayBuffer());
const served = await sha(await (await fetch(`${url}/web/app.js`, {cache: "no-store"})).arrayBuffer());
if (local !== served) {
  console.error(`the server still serves another app.js (${served.slice(0, 12)} vs ${local.slice(0, 12)}): is serve/web mounted as a directory?`);
  process.exit(1);
}
console.log(`deployed: ${url} serves app.js ${local.slice(0, 12)}; reload the page`);
