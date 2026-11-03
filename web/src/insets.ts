// The installed app's geometry. iOS reports the safe areas through CSS only,
// and a standalone app has no inspector at hand, so `?insets` prints what the
// phone actually says.

const KEY = "revenant.insets";

// A home screen app has no address bar to type `?insets` into, and does not
// share Safari's storage, so a three-finger tap inside it toggles the overlay
// and remembers the choice.
export function wantsInsets(search: string, store: Pick<Storage, "getItem"> | null): boolean {
  if (new URLSearchParams(search).has("insets")) return true;
  try {
    return store?.getItem(KEY) === "1";
  } catch {
    return false;
  }
}

export function insetsToggle(doc: Document, win: Window): void {
  doc.addEventListener(
    "touchstart",
    (ev) => {
      if (ev.touches.length !== 3) return;
      try {
        const on = win.localStorage.getItem(KEY) === "1";
        if (on) win.localStorage.removeItem(KEY);
        else win.localStorage.setItem(KEY, "1");
      } catch {
        // No storage: the query string still works.
      }
      win.location.reload();
    },
    { passive: true },
  );
}

export function showInsets(doc: Document, win: Window): void {
  const probe = doc.createElement("div");
  probe.style.cssText =
    "position:fixed;visibility:hidden;pointer-events:none;" +
    "padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)";
  const box = doc.createElement("pre");
  box.className = "insets-debug";
  doc.body.append(probe, box);
  const render = () => {
    const cs = win.getComputedStyle(probe);
    const vv = win.visualViewport;
    const nav = win.navigator as Navigator & { standalone?: boolean };
    box.textContent = [
      `inset t ${cs.paddingTop} r ${cs.paddingRight} b ${cs.paddingBottom} l ${cs.paddingLeft}`,
      `innerHeight ${win.innerHeight}  clientHeight ${doc.documentElement.clientHeight}`,
      `visualViewport.height ${vv ? vv.height.toFixed(1) : "n/a"}  offsetTop ${vv ? vv.offsetTop.toFixed(1) : "n/a"}`,
      `scrollY ${win.scrollY}  screen ${win.screen.width}x${win.screen.height}`,
      `standalone ${nav.standalone === true}`,
      `--app-top ${doc.documentElement.style.getPropertyValue("--app-top") || "unset"}  --app-h ${doc.documentElement.style.getPropertyValue("--app-h") || "unset"}`,
    ].join("\n");
  };
  render();
  win.addEventListener("resize", render);
  win.addEventListener("scroll", render);
  win.visualViewport?.addEventListener("resize", render);
  win.visualViewport?.addEventListener("scroll", render);
}

// Where the glass is inside the layout viewport. An installed app on iOS 27
// reports a viewport taller than the screen (innerHeight 1052 on a 956 tall
// screen): it reaches a top inset above the glass and a bottom inset below
// it, so a fixed box at top 0 starts under the status bar, out of sight. When
// the overshoot is exactly the two insets the glass starts a top inset down;
// otherwise nothing is assumed and the viewport is taken as it is.
export interface Geometry {
  topPx: number;
  heightPx: number;
}

export function appGeometry(g: {
  innerHeight: number;
  screenTall: number;
  insetTop: number;
  insetBottom: number;
}): Geometry {
  const overshoot = g.innerHeight - g.screenTall;
  if (g.screenTall > 0 && overshoot > 0 && Math.abs(g.insetTop + g.insetBottom - overshoot) < 1) {
    return { topPx: Math.round(overshoot - g.insetBottom), heightPx: Math.round(g.screenTall) };
  }
  return { topPx: 0, heightPx: Math.round(g.innerHeight) };
}

// The screen's length along the current orientation.
export function screenTallPx(width: number, height: number, landscape: boolean): number {
  return landscape ? Math.min(width, height) : Math.max(width, height);
}

// env() insets are only readable through CSS, so a hidden probe carries them.
function insetProbe(doc: Document): HTMLElement {
  const probe = doc.createElement("div");
  probe.style.cssText =
    "position:fixed;visibility:hidden;pointer-events:none;" +
    "padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)";
  doc.body.append(probe);
  return probe;
}

export function trackAppGeometry(win: Window): void {
  const doc = win.document;
  const probe = insetProbe(doc);
  const landscape = win.matchMedia("(orientation: landscape)");
  const set = () => {
    const cs = win.getComputedStyle(probe);
    const g = appGeometry({
      innerHeight: win.innerHeight,
      screenTall: screenTallPx(win.screen.width, win.screen.height, landscape.matches),
      insetTop: parseFloat(cs.paddingTop) || 0,
      insetBottom: parseFloat(cs.paddingBottom) || 0,
    });
    doc.documentElement.style.setProperty("--app-top", `${g.topPx}px`);
    doc.documentElement.style.setProperty("--app-h", `${g.heightPx}px`);
  };
  set();
  win.addEventListener("resize", set);
  win.visualViewport?.addEventListener("resize", set);
  landscape.addEventListener("change", set);
}

// The page itself never scrolls, but iOS scrolls the layout viewport to keep
// a focused input above the keyboard and does not always scroll it back when
// the keyboard closes. Every fixed element then sits that far up: under the
// status bar at the top, with an empty band at the bottom. Putting the scroll
// back to zero undoes it.
// While something takes typing the scroll is iOS keeping it above the
// keyboard, and undoing it would hide the field; the reset waits for blur.
export function isEditable(el: { tagName?: string; type?: string; isContentEditable?: boolean } | null): boolean {
  if (!el) return false;
  if (el.isContentEditable) return true;
  const tag = (el.tagName ?? "").toUpperCase();
  if (tag === "TEXTAREA" || tag === "SELECT") return true;
  if (tag !== "INPUT") return false;
  const t = (el.type ?? "text").toLowerCase();
  return !["button", "submit", "reset", "checkbox", "radio", "range", "color", "file", "image"].includes(t);
}

export function pinViewport(win: Window): void {
  const reset = () => {
    if (isEditable(win.document.activeElement as HTMLInputElement | null)) return;
    if (win.scrollY !== 0 || win.scrollX !== 0) win.scrollTo(0, 0);
  };
  win.addEventListener("scroll", reset, { passive: true });
  win.visualViewport?.addEventListener("resize", reset);
  win.document.addEventListener("focusout", () => win.setTimeout(reset, 0));
}
