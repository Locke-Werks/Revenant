// Service worker: caches the app shell, nothing else. build.mjs bundles this
// to dist/sw.js and defines __CACHE_NAME__ and __SHELL_FILES__, so the cache
// name changes whenever any shipped file does and old shells are dropped.

// The worker scope's surface this file touches, declared here because the DOM
// lib and the WebWorker lib cannot be loaded together.
export interface FetchRequestLike {
  method: string;
  url: string;
  mode?: string;
}
export interface FetchEventLike {
  request: FetchRequestLike;
  respondWith(response: Promise<unknown>): void;
}
export interface WaitableEventLike {
  waitUntil(p: Promise<unknown>): void;
}
export interface CacheLike {
  addAll(requests: unknown[]): Promise<void>;
  match(req: unknown, opts?: { ignoreSearch?: boolean }): Promise<unknown>;
}
export interface CachesLike {
  open(name: string): Promise<CacheLike>;
  keys(): Promise<string[]>;
  delete(name: string): Promise<boolean>;
}
export interface WorkerScope {
  location: { origin: string };
  caches: CachesLike;
  fetch(req: unknown): Promise<unknown>;
  skipWaiting?(): Promise<void>;
  clients?: { claim(): Promise<void> };
  addEventListener(type: "install" | "activate", fn: (e: WaitableEventLike) => void): void;
  addEventListener(type: "fetch", fn: (e: FetchEventLike) => void): void;
}

// Builds the request that fills the cache. A plain URL string could be answered
// from the HTTP cache, pinning a stale file under the new cache name; reload
// forces the network. Injected so tests need no Request global.
export type MakeRequest = (url: string) => unknown;
const reloadRequest: MakeRequest = (url) => new Request(url, { cache: "reload" });

const CACHE_PREFIX = "revenant-";

// True only for same-origin GETs that are not the socket or the audio stream.
// Anything else must reach the network untouched: a cached /ws upgrade or HLS
// playlist would be silently stale, and a non-GET has side effects.
export function shouldHandle(req: FetchRequestLike, origin: string): boolean {
  if (req.method !== "GET") return false;
  let url: URL;
  try {
    url = new URL(req.url);
  } catch {
    return false;
  }
  if (url.origin !== origin) return false;
  if (url.pathname === "/ws" || url.pathname.startsWith("/ws/")) return false;
  if (url.pathname.startsWith("/hls/")) return false;
  return true;
}

// The reachability probe in offline.ts: the manifest with a probe parameter.
// It must see the network, or "reachable" would always be true. Nothing else
// gets this exemption, so a stray ?probe on a shell file is still cached.
export function isProbe(req: FetchRequestLike): boolean {
  const url = new URL(req.url);
  return url.pathname === "/manifest.webmanifest" && url.searchParams.has("probe");
}

export function installWorker(
  scope: WorkerScope,
  cacheName: string,
  shellFiles: string[],
  makeRequest: MakeRequest = reloadRequest,
): void {
  scope.addEventListener("install", (e) => {
    e.waitUntil(
      scope.caches
        .open(cacheName)
        .then((c) => c.addAll(shellFiles.map(makeRequest)))
        .then(() => scope.skipWaiting?.()),
    );
  });

  scope.addEventListener("activate", (e) => {
    e.waitUntil(
      scope.caches
        .keys()
        .then((names) =>
          Promise.all(
            names
              .filter((n) => n.startsWith(CACHE_PREFIX) && n !== cacheName)
              .map((n) => scope.caches.delete(n)),
          ),
        )
        .then(() => scope.clients?.claim()),
    );
  });

  scope.addEventListener("fetch", (e) => {
    if (!shouldHandle(e.request, scope.location.origin)) return;
    if (isProbe(e.request)) return;
    e.respondWith(
      // Cache first for the shell, from this build's cache only: a leftover
      // cache from an older build must never answer. Navigations fall back to
      // the cached index so the app opens with webgate down and can show its
      // own offline screen.
      scope.caches
        .open(cacheName)
        .then((cache) =>
          cache.match(e.request, { ignoreSearch: true }).then((hit) => {
            if (hit) return hit;
            return scope.fetch(e.request).catch(() =>
              e.request.mode === "navigate" ? cache.match("/index.html") : Promise.reject(new Error("offline")),
            );
          }),
        ),
    );
  });
}

declare const __CACHE_NAME__: string;
declare const __SHELL_FILES__: string[];

// Under node (tests) there is no worker scope to attach to.
if (typeof __CACHE_NAME__ !== "undefined") {
  installWorker(self as unknown as WorkerScope, __CACHE_NAME__, __SHELL_FILES__);
}
