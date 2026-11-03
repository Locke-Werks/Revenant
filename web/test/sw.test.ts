import { test } from "node:test";
import assert from "node:assert/strict";
import {
  installWorker,
  shouldHandle,
  type FetchEventLike,
  type WaitableEventLike,
  type WorkerScope,
} from "../src/sw";

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

const CURRENT = "revenant-new";

// `cached` holds pathnames present in the current build's cache; `stale` holds
// pathnames present only in an older cache, which must never answer.
function fakeScope(cached: Set<string>, stale: Set<string> = new Set()) {
  const handlers: Record<string, (e: never) => void> = {};
  const deleted: string[] = [];
  const added: unknown[][] = [];
  const opened: string[] = [];
  const calls = { skipWaiting: 0, claim: 0 };
  const pathOf = (req: unknown) => new URL(typeof req === "string" ? req : (req as { url: string }).url, ORIGIN).pathname;
  const scope: WorkerScope = {
    location: { origin: ORIGIN },
    caches: {
      open: async (name) => {
        opened.push(name);
        const set = name === CURRENT ? cached : stale;
        return {
          addAll: async (reqs) => void added.push(reqs),
          match: async (req) => (set.has(pathOf(req)) ? "cached" : undefined),
        };
      },
      keys: async () => ["revenant-old", CURRENT, "unrelated"],
      delete: async (n) => {
        deleted.push(n);
        return true;
      },
    },
    fetch: async () => {
      throw new TypeError("offline");
    },
    skipWaiting: async () => void calls.skipWaiting++,
    clients: { claim: async () => void calls.claim++ },
    addEventListener: ((type: string, fn: (e: never) => void) => void (handlers[type] = fn)) as WorkerScope["addEventListener"],
  };
  return { scope, handlers, deleted, added, opened, calls };
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

async function waitFor(handlers: Record<string, (e: never) => void>, type: string) {
  let waited: Promise<unknown> = Promise.resolve();
  const ev: WaitableEventLike = { waitUntil: (p) => void (waited = p) };
  handlers[type]!(ev as never);
  await waited;
}

test("fetch handler does not respond to /ws, /hls/x or POST", () => {
  const { scope, handlers } = fakeScope(new Set(["/ws", "/hls/x"]));
  installWorker(scope, CURRENT, ["/"]);
  assert.equal(fire(handlers, get("/ws")), null);
  assert.equal(fire(handlers, get("/hls/x")), null);
  assert.equal(fire(handlers, { method: "POST", url: ORIGIN + "/" }), null);
});

test("fetch handler serves the shell from cache and the cached index for navigations when offline", async () => {
  const { scope, handlers } = fakeScope(new Set(["/app.js", "/index.html"]));
  installWorker(scope, CURRENT, ["/"]);
  assert.equal(await fire(handlers, get("/app.js")), "cached");
  assert.equal(await fire(handlers, get("/anything", "navigate")), "cached");
  await assert.rejects(fire(handlers, get("/missing.js"))!);
});

test("fetch handler answers only from the current build's cache", async () => {
  const { scope, handlers, opened } = fakeScope(new Set(), new Set(["/app.js"]));
  installWorker(scope, CURRENT, ["/"]);
  await assert.rejects(fire(handlers, get("/app.js"))!);
  assert.deepEqual(opened, [CURRENT]);
});

test("only the manifest probe goes to the network; /app.js?probe is cached", async () => {
  const { scope, handlers } = fakeScope(new Set(["/manifest.webmanifest", "/app.js"]));
  installWorker(scope, CURRENT, ["/"]);
  assert.equal(fire(handlers, get("/manifest.webmanifest?probe=1")), null);
  assert.equal(await fire(handlers, get("/app.js?probe=1")), "cached");
  assert.equal(await fire(handlers, get("/manifest.webmanifest")), "cached");
});

test("install fills the cache with reload requests for every shell file, then skipWaiting", async () => {
  const { scope, handlers, added, calls } = fakeScope(new Set());
  const made: string[] = [];
  installWorker(scope, CURRENT, ["/", "/app.js"], (url) => {
    made.push(url);
    return { reloadOf: url };
  });
  await waitFor(handlers, "install");
  assert.deepEqual(made, ["/", "/app.js"]);
  assert.deepEqual(added, [[{ reloadOf: "/" }, { reloadOf: "/app.js" }]]);
  assert.equal(calls.skipWaiting, 1);
});

test("activate deletes older revenant caches only, then claims clients", async () => {
  const { scope, handlers, deleted, calls } = fakeScope(new Set());
  installWorker(scope, CURRENT, ["/"]);
  await waitFor(handlers, "activate");
  assert.deepEqual(deleted, ["revenant-old"]);
  assert.equal(calls.claim, 1);
  assert.equal(calls.skipWaiting, 0);
});
