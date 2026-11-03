// Bundles each test with esbuild, then runs node's built-in runner over the
// output. Bundling resolves the extensionless TypeScript imports without
// adding a loader or a second test dependency.
import { build } from "esbuild";
import { spawnSync } from "node:child_process";
import { readdirSync, rmSync } from "node:fs";

const out = ".test-out";
rmSync(out, { recursive: true, force: true });
const entries = readdirSync("test").filter((f) => f.endsWith(".test.ts")).map((f) => `test/${f}`);
await build({
  entryPoints: entries,
  outdir: out,
  bundle: true,
  platform: "node",
  format: "esm",
  external: ["node:*"],
  outExtension: { ".js": ".mjs" },
});
// node 22 does not accept a directory here, so name the bundles.
const bundles = entries.map((e) => `${out}/${e.slice("test/".length, -".ts".length)}.mjs`);
const r = spawnSync(process.execPath, ["--test", ...bundles], { stdio: "inherit" });
process.exit(r.status ?? 1);
