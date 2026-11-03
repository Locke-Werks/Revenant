import { chooseScreen, installScreenHtml, type StandaloneEnv } from "./install";
import { gatewayReachable, offlineScreenHtml, type Probe } from "./offline";
import { appScreenHtml, mountApp } from "./app";

export interface Host {
  root: { innerHTML: string; querySelector(sel: string): { addEventListener(ev: string, fn: () => void): void } | null };
  env: StandaloneEnv;
  search: string;
  probe: Probe;
  // Starts the app once its markup is in place. Absent under node.
  mount?: () => void;
}

export async function render(host: Host): Promise<void> {
  if (chooseScreen(host.env, host.search) === "install") {
    host.root.innerHTML = installScreenHtml();
    return;
  }
  if (await gatewayReachable(host.probe)) {
    host.root.innerHTML = appScreenHtml();
    host.mount?.();
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
    // A home screen app is resumed far more often than it is launched, and the
    // browser only checks for a new worker on navigation. Checking when the app
    // comes back to the foreground is what lets an update land.
    void navigator.serviceWorker.register("/sw.js").then((reg) => {
      document.addEventListener("visibilitychange", () => {
        if (document.visibilityState === "visible") void reg.update().catch(() => {});
      });
    });
  }
  const root = document.getElementById("app");
  if (root) {
    void render({
      root,
      env: { navigator: navigator as { standalone?: boolean }, matchMedia: (q) => window.matchMedia(q) },
      search: location.search,
      probe: (input, init) => fetch(input, init),
      mount: () => void mountApp(root),
    });
  }
}
