// A new service worker takes control of a running page, but the page keeps
// the old bundle it already loaded, so an update would need a second launch
// to show. Reloading once when control changes lands it on the first.
//
// Not on the first claim of a page that had no worker (a fresh install is
// already running the new bundle), not twice in quick succession (a worker
// that keeps replacing itself must not loop the page), and never under a
// finger or a cursor: a reload mid-drag or mid-typing loses what was being
// done, so it waits for the page to go idle.

export const RELOAD_GUARD_MS = 10_000;
export const IDLE_RETRY_MS = 1_000;
const KEY = "revenant.swReloadAt";

export interface ReloadDeps {
  // Whether a worker controlled the page when it loaded.
  controlled: boolean;
  onControllerChange(fn: () => void): void;
  busy(): boolean;
  reload(): void;
  now(): number;
  setTimeout(fn: () => void, ms: number): void;
  store: Pick<Storage, "getItem" | "setItem"> | null;
}

export function reloadOnUpdate(d: ReloadDeps): void {
  let controlled = d.controlled;
  let pending = false;
  const attempt = () => {
    if (d.busy()) {
      d.setTimeout(attempt, IDLE_RETRY_MS);
      return;
    }
    let last = Number.NaN;
    try {
      last = Number(d.store?.getItem(KEY));
    } catch {
      // No storage: the in-page flag still stops a second reload.
    }
    if (Number.isFinite(last) && d.now() - last < RELOAD_GUARD_MS) return;
    try {
      d.store?.setItem(KEY, String(d.now()));
    } catch {
      // As above.
    }
    d.reload();
  };
  d.onControllerChange(() => {
    if (!controlled) {
      controlled = true;
      return;
    }
    if (pending) return;
    pending = true;
    attempt();
  });
}
