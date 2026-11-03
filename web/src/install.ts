// Standalone detection and the install screen. The app only runs from the home
// screen: Safari's tab chrome, back-forward swipes and tab eviction all break a
// radio that has to keep listening, so a Safari visit gets instructions instead.

export interface StandaloneEnv {
  // iOS Safari only; absent on every other browser.
  navigator?: { standalone?: boolean };
  matchMedia?: (query: string) => { matches: boolean };
}

export function isStandalone(env: StandaloneEnv): boolean {
  if (env.navigator?.standalone === true) return true;
  return env.matchMedia?.("(display-mode: standalone)").matches === true;
}

export type Screen = "install" | "app";

// ?dev=1 exists so the shell can be worked on in a desktop browser tab.
export function chooseScreen(env: StandaloneEnv, search: string): Screen {
  if (new URLSearchParams(search).get("dev") === "1") return "app";
  return isStandalone(env) ? "app" : "install";
}

// ?dev=1&layout=1 mounts the app without asking for the gateway, so the pane
// split can be looked at from a static server with no engine behind it.
export function layoutPreview(search: string): boolean {
  const q = new URLSearchParams(search);
  return q.get("dev") === "1" && q.get("layout") === "1";
}

export function installScreenHtml(): string {
  return `
<main class="screen install">
  <img class="mark" src="/icons/icon-192.png" width="96" height="96" alt="Revenant">
  <h1>Install Revenant</h1>
  <p>Revenant runs from your home screen so it can keep listening with the phone locked.</p>
  <ol>
    <li>Tap the <strong>Share</strong> button in Safari.</li>
    <li>Choose <strong>Add to Home Screen</strong>.</li>
    <li>Open Revenant from the new icon.</li>
  </ol>
</main>`;
}
