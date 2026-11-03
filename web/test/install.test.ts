import { test } from "node:test";
import assert from "node:assert/strict";
import { isStandalone, chooseScreen, installScreenHtml } from "../src/install";
import { render, type Host } from "../src/main";
import { gatewayReachable, offlineScreenHtml } from "../src/offline";
import {
  installWorker,
  shouldHandle,
  type FetchEventLike,
  type WaitableEventLike,
  type WorkerScope,
} from "../src/sw";

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

const ORIGIN = "https://box.local:17700";
const get = (path: string, mode = "cors") => ({ method: "GET", url: ORIGIN + path, mode });

test("shouldHandle passes /ws, /hls/, non-GET and cross-origin through", () => {
  assert.equal(shouldHandle(get("/ws"), ORIGIN), false);
  assert.equal(shouldHandle(get("/hls/x"), ORIGIN), false);
  assert.equal(shouldHandle(get("/hls/a/b.m4s"), ORIGIN), false);
  assert.equal(shouldHandle({ method: "POST", url: ORIGIN + "/app.js" }, ORIGIN), false);
  assert.equal(shouldHandle({ method: "GET", url: "https://other.example/app.js" }, ORIGIN), false);
  assert.equal(shouldHandle(get("/app.js"), ORIGIN), true);
  assert.equal(shouldHandle(get("/"), ORIGIN), true);
});

function fakeScope(cached: Set<string>) {
  const handlers: Record<string, (e: never) => void> = {};
  const deleted: string[] = [];
  const scope: WorkerScope = {
    location: { origin: ORIGIN },
    caches: {
      open: async () => ({ addAll: async () => {}, match: async () => undefined, put: async () => {} }),
      keys: async () => ["revenant-old", "revenant-new", "unrelated"],
      delete: async (n) => {
        deleted.push(n);
        return true;
      },
      match: async (req) => {
        const url = typeof req === "string" ? req : (req as { url: string }).url;
        return cached.has(new URL(url, ORIGIN).pathname) ? "cached" : undefined;
      },
    },
    fetch: async () => {
      throw new TypeError("offline");
    },
    addEventListener: ((type: string, fn: (e: never) => void) => void (handlers[type] = fn)) as WorkerScope["addEventListener"],
  };
  return { scope, handlers, deleted };
}

function fire(handlers: Record<string, (e: never) => void>, req: FetchEventLike["request"]) {
  let responded: Promise<unknown> | null = null;
  const ev: FetchEventLike = {
    request: req,
    respondWith: (p) => {
      responded = p;
    },
  };
  handlers["fetch"]!(ev as never);
  return responded as Promise<unknown> | null;
}

test("fetch handler does not respond to /ws, /hls/x or POST", () => {
  const { scope, handlers } = fakeScope(new Set(["/ws", "/hls/x"]));
  installWorker(scope, "revenant-new", ["/"]);
  assert.equal(fire(handlers, get("/ws")), null);
  assert.equal(fire(handlers, get("/hls/x")), null);
  assert.equal(fire(handlers, { method: "POST", url: ORIGIN + "/" }), null);
});

test("fetch handler serves the shell from cache and the cached index for navigations when offline", async () => {
  const { scope, handlers } = fakeScope(new Set(["/app.js", "/index.html"]));
  installWorker(scope, "revenant-new", ["/"]);
  assert.equal(await fire(handlers, get("/app.js")), "cached");
  assert.equal(await fire(handlers, get("/anything", "navigate")), "cached");
  await assert.rejects(fire(handlers, get("/missing.js"))!);
});

test("fetch handler leaves the reachability probe to the network", () => {
  const { scope, handlers } = fakeScope(new Set(["/manifest.webmanifest"]));
  installWorker(scope, "revenant-new", ["/"]);
  assert.equal(fire(handlers, get("/manifest.webmanifest?probe=1")), null);
});

test("activate deletes older revenant caches only", async () => {
  const { scope, handlers, deleted } = fakeScope(new Set());
  installWorker(scope, "revenant-new", ["/"]);
  let waited: Promise<unknown> = Promise.resolve();
  const ev: WaitableEventLike = { waitUntil: (p) => void (waited = p) };
  handlers["activate"]!(ev as never);
  await waited;
  assert.deepEqual(deleted, ["revenant-old"]);
});
