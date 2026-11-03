// Builds the web app into dist/: bundles the app and the service worker, copies
// web/static, and stamps the worker with a cache name derived from the content
// of everything shipped. The gateway serves dist/ as its `web` directory.
import { build } from "esbuild";
import { createHash } from "node:crypto";
import { cpSync, mkdirSync, readFileSync, readdirSync, rmSync, statSync } from "node:fs";
import { join, relative, sep } from "node:path";

const dist = "dist";
rmSync(dist, { recursive: true, force: true });
mkdirSync(dist, { recursive: true });
cpSync("static", dist, { recursive: true });

await build({
  entryPoints: { app: "src/main.ts" },
  outdir: dist,
  bundle: true,
  format: "iife",
  target: "es2022",
  minify: true,
  sourcemap: false,
});

function walk(dir) {
  return readdirSync(dir, { withFileTypes: true }).flatMap((e) =>
    e.isDirectory() ? walk(join(dir, e.name)) : [join(dir, e.name)],
  );
}
const urlOf = (f) => "/" + relative(dist, f).split(sep).join("/");

// The worker and the splash images are not part of the offline shell: the
// worker cannot cache itself, and splash images are fetched by iOS at install.
const shell = walk(dist).filter((f) => !f.startsWith(join(dist, "splash")));
const hash = createHash("sha256");
for (const f of shell.sort()) {
  hash.update(urlOf(f));
  hash.update(readFileSync(f));
}
const cacheName = "revenant-" + hash.digest("hex").slice(0, 12);
const shellFiles = ["/", ...shell.map(urlOf)];

await build({
  entryPoints: { sw: "src/sw.ts" },
  outdir: dist,
  bundle: true,
  format: "iife",
  target: "es2022",
  minify: true,
  define: {
    __CACHE_NAME__: JSON.stringify(cacheName),
    __SHELL_FILES__: JSON.stringify(shellFiles),
  },
});

console.log(`web: ${cacheName}, ${shellFiles.length} shell files, ${walk(dist).length} total`);
