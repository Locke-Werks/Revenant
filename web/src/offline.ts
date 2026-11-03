// The "not reachable" screen. The service worker serves the shell from cache,
// so with webgate down the app still opens; this is what it shows instead of
// Safari's error page.

export type Probe = (input: string, init?: { cache?: "no-store" }) => Promise<{ ok: boolean }>;

// The manifest is small and always served no-cache, so a reply from the network
// (not the worker's cache) means the gateway is up. no-store bypasses the HTTP
// cache; the worker passes this request to the network as well.
export async function gatewayReachable(probe: Probe): Promise<boolean> {
  try {
    const r = await probe("/manifest.webmanifest?probe=" + Date.now(), { cache: "no-store" });
    return r.ok;
  } catch {
    return false;
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
