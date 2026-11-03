import { chooseScreen, installScreenHtml, type StandaloneEnv } from "./install";
import { gatewayReachable, offlineScreenHtml, type Probe } from "./offline";

// The real app arrives in later tasks; until then standalone shows a
// placeholder so the install, icon and offline paths can be checked on a phone.
export function placeholderHtml(): string {
  return `
<main class="screen app">
  <img class="mark" src="/icons/icon-192.png" width="96" height="96" alt="">
  <h1>Revenant</h1>
</main>`;
}

export interface Host {
  root: { innerHTML: string; querySelector(sel: string): { addEventListener(ev: string, fn: () => void): void } | null };
  env: StandaloneEnv;
  search: string;
  probe: Probe;
}

export async function render(host: Host): Promise<void> {
  if (chooseScreen(host.env, host.search) === "install") {
    host.root.innerHTML = installScreenHtml();
    return;
  }
  if (await gatewayReachable(host.probe)) {
    host.root.innerHTML = placeholderHtml();
    return;
  }
  host.root.innerHTML = offlineScreenHtml();
  host.root.querySelector("#retry")?.addEventListener("click", () => {
    void render(host);
  });
}

// Skipped under node, where the tests import this module without a page.
if (typeof document !== "undefined") {
  if ("serviceWorker" in navigator) {
    void navigator.serviceWorker.register("/sw.js");
  }
  const root = document.getElementById("app");
  if (root) {
    void render({
      root,
      env: { navigator: navigator as { standalone?: boolean }, matchMedia: (q) => window.matchMedia(q) },
      search: location.search,
      probe: (input, init) => fetch(input, init),
    });
  }
}
