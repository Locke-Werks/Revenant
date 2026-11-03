// The "not reachable" screen. The service worker serves the shell from cache,
// so with webgate down the app still opens; this is what it shows instead of
// Safari's error page.

export type Probe = (
  input: string,
  init?: { cache?: "no-store"; signal?: AbortSignal },
) => Promise<{ ok: boolean }>;

export const PROBE_TIMEOUT_MS = 4000;

// The manifest is small and always served no-cache, so a reply from the network
// (not the worker's cache) means the gateway is up. no-store bypasses the HTTP
// cache; the worker passes this request to the network as well.
//
// With the gateway down, iOS can leave a fetch pending for tens of seconds on a
// blank page. The timer both aborts the request and settles the race itself,
// so a probe that ignores its signal still resolves as unreachable.
export async function gatewayReachable(probe: Probe, timeoutMs: number = PROBE_TIMEOUT_MS): Promise<boolean> {
  const ctl = new AbortController();
  let timer: ReturnType<typeof setTimeout> | undefined;
  const timeout = new Promise<false>((resolve) => {
    timer = setTimeout(() => {
      ctl.abort();
      resolve(false);
    }, timeoutMs);
  });
  const attempt = probe("/manifest.webmanifest?probe=" + Date.now(), { cache: "no-store", signal: ctl.signal }).then(
    (r) => r.ok,
    () => false,
  );
  try {
    return await Promise.race([attempt, timeout]);
  } finally {
    clearTimeout(timer);
  }
}

export function offlineScreenHtml(): string {
  return `
<main class="screen offline">
  <img class="mark" src="/icons/icon-192.png" width="96" height="96" alt="Revenant">
  <h1>Revenant is not reachable</h1>
  <p>The radio box did not answer. Check that it is on and that this phone is on the same network.</p>
  <button type="button" id="retry">Retry</button>
</main>`;
}
