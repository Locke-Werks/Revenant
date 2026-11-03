import { test } from "node:test";
import assert from "node:assert/strict";
import { isStandalone, chooseScreen, installScreenHtml } from "../src/install";
import { render, type Host } from "../src/main";
import { gatewayReachable, offlineScreenHtml } from "../src/offline";

const media = (matches: boolean) => (q: string) => ({ matches: matches && q === "(display-mode: standalone)" });

test("isStandalone reads navigator.standalone", () => {
  assert.equal(isStandalone({ navigator: { standalone: true } }), true);
  assert.equal(isStandalone({ navigator: { standalone: false } }), false);
});

test("isStandalone reads the display-mode media query", () => {
  assert.equal(isStandalone({ matchMedia: media(true) }), true);
  assert.equal(isStandalone({ matchMedia: media(false) }), false);
});

test("isStandalone is false with no signal at all", () => {
  assert.equal(isStandalone({}), false);
  assert.equal(isStandalone({ navigator: {}, matchMedia: media(false) }), false);
});

test("install screen when not standalone and no dev flag", () => {
  assert.equal(chooseScreen({ navigator: { standalone: false } }, ""), "install");
  assert.equal(chooseScreen({ navigator: { standalone: false } }, "?dev=0"), "install");
});

test("dev=1 bypasses the install screen; standalone shows the app", () => {
  assert.equal(chooseScreen({}, "?dev=1"), "app");
  assert.equal(chooseScreen({ navigator: { standalone: true } }, ""), "app");
});

function host(opts: { standalone: boolean; reachable: boolean }) {
  const listeners: Record<string, () => void> = {};
  const root = {
    innerHTML: "",
    querySelector: (sel: string) =>
      sel === "#retry" ? { addEventListener: (ev: string, fn: () => void) => void (listeners[ev] = fn) } : null,
  };
  const state = { reachable: opts.reachable };
  const h: Host = {
    root,
    env: { navigator: { standalone: opts.standalone } },
    search: "",
    probe: async () => {
      if (!state.reachable) throw new TypeError("network down");
      return { ok: true };
    },
  };
  return { h, root, listeners, state };
}

test("render shows the install screen in a browser tab", async () => {
  const { h, root } = host({ standalone: false, reachable: true });
  await render(h);
  assert.equal(root.innerHTML, installScreenHtml());
});

test("render shows the offline screen with Retry, and Retry recovers", async () => {
  const { h, root, listeners, state } = host({ standalone: true, reachable: false });
  await render(h);
  assert.equal(root.innerHTML, offlineScreenHtml());
  assert.match(root.innerHTML, /Retry/);
  state.reachable = true;
  listeners["click"]!();
  await new Promise((r) => setTimeout(r, 0));
  assert.match(root.innerHTML, /<h1>Revenant<\/h1>/);
});

test("gatewayReachable is false on a network error or a bad status", async () => {
  assert.equal(await gatewayReachable(async () => ({ ok: true })), true);
  assert.equal(await gatewayReachable(async () => ({ ok: false })), false);
  assert.equal(
    await gatewayReachable(async () => {
      throw new TypeError("x");
    }),
    false,
  );
});

test("gatewayReachable gives up on a probe that never resolves, and aborts it", async () => {
  let aborted = false;
  const started = Date.now();
  const ok = await gatewayReachable((_url, init) => {
    init?.signal?.addEventListener("abort", () => void (aborted = true));
    return new Promise(() => {});
  }, 30);
  assert.equal(ok, false);
  assert.equal(aborted, true);
  assert.equal(Date.now() - started < 2000, true);
});
