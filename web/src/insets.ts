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

// Labels for a ruler line every `step` CSS px from 0 down to `height`.
export function rulerMarks(height: number, step = 100): number[] {
  const out: number[] = [];
  for (let y = step; y < height; y += step) out.push(y);
  return out;
}

// The widest elements past limit, widest first. Each line names the element
// so a screenshot from the phone says what is pushing the page out.
export function widest(items: { label: string; width: number }[], limit: number, n = 8): string[] {
  return items
    .filter((x) => x.width > limit + 0.5)
    .sort((a, b) => b.width - a.width)
    .slice(0, n)
    .map((x) => `${x.label} ${x.width.toFixed(1)}`);
}

function overWide(doc: Document, limit: number): string[] {
  const items: { label: string; width: number }[] = [];
  for (const el of Array.from(doc.querySelectorAll("*"))) {
    const cls = typeof el.className === "string" && el.className ? "." + el.className.trim().split(/\s+/).join(".") : "";
    const label = el.tagName.toLowerCase() + (el.id ? "#" + el.id : "") + cls;
    items.push({ label, width: Math.max(el.getBoundingClientRect().width, el.scrollWidth) });
  }
  return widest(items, limit);
}

function rectText(name: string, r: { top: number; bottom: number; height: number }): string {
  return `${name} top ${r.top.toFixed(1)} bottom ${r.bottom.toFixed(1)} h ${r.height.toFixed(1)}`;
}

// A layout ruler drawn inside the app: what the viewport, the app box and the
// sheet's handle really occupy, in the page's own coordinates, so a screenshot
// from the phone settles where things are instead of guessing. Touches pass
// through everything but the close button.
export function showInsets(doc: Document, win: Window): void {
  // Before anything of the overlay exists, so its own width is not counted.
  const wideBefore = overWide(doc, win.screen.width);
  const layer = doc.createElement("div");
  layer.className = "insets-layer";
  layer.style.cssText = "position:fixed;top:0;left:0;right:0;bottom:0;z-index:100;pointer-events:none;";
  doc.body.append(layer);

  const probe = doc.createElement("div");
  probe.style.cssText =
    "position:fixed;visibility:hidden;pointer-events:none;" +
    "padding:env(safe-area-inset-top) env(safe-area-inset-right) env(safe-area-inset-bottom) env(safe-area-inset-left)";
  const units = ["100dvh", "100lvh", "100svh", "100vh"].map((u) => {
    const p = doc.createElement("div");
    p.style.cssText = `position:absolute;visibility:hidden;pointer-events:none;width:1px;height:${u}`;
    return { u, p };
  });
  doc.body.append(probe, ...units.map((x) => x.p));

  // (a) A box pinned to all four viewport edges with no height of its own.
  const frame = doc.createElement("div");
  frame.style.cssText = "position:fixed;top:0;bottom:0;left:0;right:0;outline:2px solid red;outline-offset:-2px;pointer-events:none;";
  const frameText = doc.createElement("div");
  frameText.style.cssText = "position:absolute;left:4px;bottom:4px;color:red;font:11px/1.2 ui-monospace,monospace;white-space:normal;overflow-wrap:anywhere;max-width:calc(100vw - 16px);background:rgba(0,0,0,0.7);";
  frame.append(frameText);

  const box = (color: string) => {
    const b = doc.createElement("div");
    b.style.cssText = `position:fixed;outline:2px solid ${color};outline-offset:-2px;pointer-events:none;`;
    const t = doc.createElement("div");
    t.style.cssText = `position:absolute;right:4px;top:4px;color:${color};font:11px/1.2 ui-monospace,monospace;white-space:normal;overflow-wrap:anywhere;max-width:calc(100vw - 16px);background:rgba(0,0,0,0.7);`;
    b.append(t);
    return { b, t };
  };
  const appBox = box("#4af");
  const grabBox = box("#4f4");

  const lines = doc.createElement("div");
  const text = doc.createElement("pre");
  text.className = "insets-debug";
  const close = doc.createElement("button");
  close.type = "button";
  close.textContent = "Close ruler";
  close.style.cssText = "position:fixed;left:50%;top:22%;transform:translateX(-50%);pointer-events:auto;z-index:101;margin:0;";
  close.addEventListener("click", () => {
    try {
      win.localStorage.removeItem(KEY);
    } catch {
      // The query string still turns it on.
    }
    layer.remove();
  });
  layer.append(lines, frame, appBox.b, grabBox.b, text, close);

  const place = (target: { b: HTMLElement; t: HTMLElement }, name: string, el: Element | null) => {
    if (!el) {
      target.b.hidden = true;
      return;
    }
    const r = el.getBoundingClientRect();
    target.b.hidden = false;
    Object.assign(target.b.style, { top: `${r.top}px`, left: `${r.left}px`, width: `${r.width}px`, height: `${r.height}px` });
    target.t.textContent = rectText(name, r);
  };

  const render = () => {
    const fr = frame.getBoundingClientRect();
    frameText.textContent = rectText("fixed 0/0", fr);
    lines.textContent = "";
    for (const y of rulerMarks(Math.max(fr.bottom, win.innerHeight, win.screen.height))) {
      const l = doc.createElement("div");
      l.style.cssText = `position:fixed;left:0;right:0;top:${y}px;height:1px;background:rgba(255,255,0,0.6);pointer-events:none;`;
      const lab = doc.createElement("span");
      lab.textContent = String(y);
      lab.style.cssText = "position:absolute;left:2px;top:-12px;color:#ff0;font:10px ui-monospace,monospace;";
      l.append(lab);
      lines.append(l);
    }
    place(appBox, "app", doc.querySelector(".app"));
    place(grabBox, "grab", doc.querySelector(".sheet:not([hidden])") ?? doc.querySelector(".grabber"));
    const cs = win.getComputedStyle(probe);
    const vv = win.visualViewport;
    const nav = win.navigator as Navigator & { standalone?: boolean };
    const root = doc.documentElement;
    text.textContent = [
      `inset t ${cs.paddingTop} r ${cs.paddingRight} b ${cs.paddingBottom} l ${cs.paddingLeft}`,
      `innerHeight ${win.innerHeight}  outerHeight ${win.outerHeight}  clientHeight ${root.clientHeight}`,
      `visualViewport.height ${vv ? vv.height.toFixed(1) : "n/a"}  offsetTop ${vv ? vv.offsetTop.toFixed(1) : "n/a"}`,
      `scrollY ${win.scrollY}  screen ${win.screen.width}x${win.screen.height}  availHeight ${win.screen.availHeight}`,
      units.map((x) => `${x.u} ${x.p.getBoundingClientRect().height.toFixed(1)}`).join("  "),
      `standalone ${nav.standalone === true}`,
      `wider than screen at open: ${wideBefore.join("; ") || "none"}`,
      `wider than screen now: ${overWide(doc, win.screen.width).join("; ") || "none"}`,
      `scale ${vv ? vv.scale.toFixed(3) : "n/a"}  scrollWidth ${root.scrollWidth}  innerWidth ${win.innerWidth}`,
    ].join("\n");
  };
  render();
  for (const t of ["resize", "scroll", "orientationchange"]) win.addEventListener(t, render);
  win.visualViewport?.addEventListener("resize", render);
  win.visualViewport?.addEventListener("scroll", render);
  // The app and its sheet mount after this runs and move when the sheet opens.
  win.setInterval(render, 1000);
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
