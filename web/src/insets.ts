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
    ].join("\n");
  };
  render();
  win.addEventListener("resize", render);
  win.addEventListener("scroll", render);
  win.visualViewport?.addEventListener("resize", render);
  win.visualViewport?.addEventListener("scroll", render);
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
