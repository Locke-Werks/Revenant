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

function wideAtLoad(win: Window): string {
  const w = (win as unknown as { __wideAtLoad?: { maxScrollWidth: number; at: number; items: { label: string; width: number; css: string }[] } }).__wideAtLoad;
  if (!w) return "not recorded";
  const items = w.items.map((x) => `${x.label} ${x.width.toFixed(1)} (${x.css})`).join("; ");
  return `scrollWidth ${w.maxScrollWidth} at ${w.at} ms: ${items || "nothing past the screen"}`;
}

// Launches recorded by the head script in index.html, newest last. The
// current launch writes its own entry 3 s in, so the newest without the
// overlay that was written before this page loaded is the previous one.
export interface LaunchCapture {
  t: number;
  overlay: boolean;
  maxScrollWidth: number;
  at: number;
  items: { label: string; width: number; css: string }[];
  scale: number | null;
  innerWidth: number;
  innerHeight?: number;
  screenWidth?: number;
  screenHeight?: number;
  insets?: { top: number; bottom: number } | null;
  savedAt?: number;
}

export function previousNormal(list: LaunchCapture[], loadedAt: number): LaunchCapture | null {
  for (let i = list.length - 1; i >= 0; i--) if (!list[i]!.overlay && list[i]!.t < loadedAt) return list[i]!;
  return null;
}

function previousLaunch(win: Window): string {
  let list: LaunchCapture[] = [];
  try {
    list = JSON.parse(win.localStorage.getItem("revenant.wideLaunches") ?? "[]") as LaunchCapture[];
  } catch {
    return "not stored";
  }
  const loadedAt = Date.now() - win.performance.now();
  const p = previousNormal(Array.isArray(list) ? list : [], loadedAt);
  if (!p) return "none recorded";
  const items = p.items.map((x) => `${x.label} ${x.width.toFixed(1)} (${x.css})`).join("; ");
  const ins = p.insets ? `t ${p.insets.top} b ${p.insets.bottom}` : "n/a";
  return (
    `at ${p.savedAt ?? "?"} ms: inner ${p.innerWidth}x${p.innerHeight ?? "?"} scale ${p.scale ?? "n/a"} ` +
    `screen ${p.screenWidth ?? "?"}x${p.screenHeight ?? "?"} insets ${ins}; widest ${p.maxScrollWidth} at ${p.at} ms: ${items || "nothing past the screen"}`
  );
}

function repins(win: Window): string {
  const log = (win as unknown as { __repins?: Repin[] }).__repins ?? [];
  return log.map((r) => `${r.at} ms ${r.before?.toFixed(3) ?? "n/a"} -> ${r.after?.toFixed(3) ?? "n/a"}`).join("; ") || "none";
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
  layer.style.cssText = "position:fixed;top:0;left:0;right:0;bottom:0;z-index:100;pointer-events:none;overflow:hidden;";
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
  // Inline, so the text wraps from the moment it is inserted, before app.css
  // rules could be late; unwrapped it widened the page to 1137 px at load.
  text.style.cssText = "white-space:pre-wrap;overflow-wrap:anywhere;max-width:calc(100vw - 16px);box-sizing:border-box;";
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
      `widest during load: ${wideAtLoad(win)}`,
      `--top-clear ${win.getComputedStyle(root).getPropertyValue("--top-clear").trim() || "unset"}  --short-fix ${doc.documentElement.style.getPropertyValue("--short-fix") || "unset"}  deficit ${((Math.max(win.screen.width, win.screen.height) / (vv?.scale || 1)) - win.innerHeight).toFixed(1)} (portrait)`,
      `short-viewport reload: ${shortReloadText(win)}`,
      `last launch without ruler: ${previousLaunch(win)}`,
      `viewport re-pins: ${repins(win)}`,
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

// The layout viewport is wider than the device when the page is zoomed out or
// the root is wider than the screen's width along the current orientation.
export function needsRepin(g: { scale: number | null; clientWidth: number; screenWidth: number; screenHeight: number; landscape: boolean }): boolean {
  const deviceWidth = g.landscape ? Math.max(g.screenWidth, g.screenHeight) : Math.min(g.screenWidth, g.screenHeight);
  if (g.scale !== null && g.scale < 0.99) return true;
  return deviceWidth > 0 && g.clientWidth > deviceWidth + 0.5;
}

export interface Repin {
  at: number;
  before: number | null;
  after: number | null;
}

// iOS settles the layout viewport's width early and keeps it; changing the
// viewport meta makes it work the width out again. Checked after mount and
// after every size change, settled first so a rotation is not caught halfway.
export function pinViewportWidth(win: Window): () => void {
  const doc = win.document;
  const log: Repin[] = [];
  (win as unknown as { __repins: Repin[] }).__repins = log;
  let timer = 0;
  const check = () => {
    const meta = doc.querySelector<HTMLMetaElement>('meta[name="viewport"]');
    if (!meta) return;
    const landscape = win.matchMedia("(orientation: landscape)").matches;
    const scale = win.visualViewport?.scale ?? null;
    const wide = needsRepin({
      scale,
      clientWidth: doc.documentElement.clientWidth,
      screenWidth: win.screen.width,
      screenHeight: win.screen.height,
      landscape,
    });
    if (!wide || log.length >= 10) return;
    const content = meta.getAttribute("content") ?? "";
    meta.setAttribute("content", "");
    win.requestAnimationFrame(() => {
      meta.setAttribute("content", content);
      win.requestAnimationFrame(() => log.push({ at: Math.round(win.performance.now()), before: scale, after: win.visualViewport?.scale ?? null }));
    });
  };
  const later = () => {
    win.clearTimeout(timer);
    timer = win.setTimeout(check, 250);
  };
  win.addEventListener("resize", later);
  win.addEventListener("orientationchange", later);
  win.visualViewport?.addEventListener("resize", later);
  return later;
}

// The iOS 26+ standalone short viewport: on a cold launch the layout viewport
// can stop short of the bottom of the glass (a reload then gets it right).
// The screen is in points and the viewport in CSS px, which part ways under
// Safari's page zoom (the owner runs 85%), so the shortfall is measured in
// CSS px through the scale. Returns how far to extend the app, 0 when the
// viewport already reaches the glass or the gap is too large to be this bug.
export function shortViewportFix(g: {
  standalone: boolean;
  innerHeight: number;
  scale: number | null;
  screenWidth: number;
  screenHeight: number;
  landscape: boolean;
}): number {
  if (!g.standalone) return 0;
  const scale = g.scale !== null && g.scale > 0 ? g.scale : 1;
  const tall = g.landscape ? Math.min(g.screenWidth, g.screenHeight) : Math.max(g.screenWidth, g.screenHeight);
  const deficit = tall / scale - g.innerHeight;
  return deficit > 1 && deficit < 200 ? Math.round(deficit * 10) / 10 : 0;
}

// A cold launch can get the short viewport where a reload gets the full one,
// so the cure is one reload before anything live has started. Once tried,
// a viewport that is still short keeps the --short-fix extension instead.
export type LaunchAction = "reload" | "extend" | "none";

export function launchAction(deficit: number, reloadedAlready: boolean): LaunchAction {
  if (!(deficit > 1 && deficit < 200)) return "none";
  return reloadedAlready ? "extend" : "reload";
}

const SHORT_RELOAD_KEY = "revenant.shortReload";
// Shared with sw_reload.ts so the two reload paths cannot both fire.
export const RELOAD_AT_KEY = "revenant.swReloadAt";

export function currentDeficit(win: Window): number {
  if ((win.navigator as Navigator & { standalone?: boolean }).standalone !== true) return 0;
  return shortViewportFix({
    standalone: true,
    innerHeight: win.innerHeight,
    scale: win.visualViewport?.scale ?? null,
    screenWidth: win.screen.width,
    screenHeight: win.screen.height,
    landscape: win.matchMedia("(orientation: landscape)").matches,
  });
}

// Runs first thing at launch. True when the page is reloading and nothing
// else should start. The outcome of each try is kept for the ruler.
export function reloadIfShort(win: Window): boolean {
  let session: Storage | null = null;
  try {
    session = win.sessionStorage;
  } catch {
    return false;
  }
  const deficit = currentDeficit(win);
  const tried = session.getItem(SHORT_RELOAD_KEY);
  const action = launchAction(deficit, tried !== null);
  if (tried !== null && session.getItem("revenant.shortReloadLogged") === null) {
    session.setItem("revenant.shortReloadLogged", "1");
    try {
      win.localStorage.setItem("revenant.shortReloadLog", JSON.stringify({ t: Date.now(), before: Number(tried), after: deficit }));
    } catch {
      // The ruler just has less to say.
    }
  }
  if (action !== "reload") return false;
  session.setItem(SHORT_RELOAD_KEY, String(deficit));
  session.setItem(RELOAD_AT_KEY, String(Date.now()));
  win.location.reload();
  return true;
}

export function shortReloadText(win: Window): string {
  try {
    const raw = win.localStorage.getItem("revenant.shortReloadLog");
    if (!raw) return "never";
    const r = JSON.parse(raw) as { t: number; before: number; after: number };
    return `${new Date(r.t).toLocaleString()}: deficit ${r.before} -> ${r.after}`;
  } catch {
    return "not stored";
  }
}

// iOS may correct the viewport later, after which the fix measures 0 and
// turns itself off.
export function trackShortViewport(win: Window): void {
  const doc = win.document;
  const landscape = win.matchMedia("(orientation: landscape)");
  const set = () => {
    const fix = shortViewportFix({
      standalone: (win.navigator as Navigator & { standalone?: boolean }).standalone === true,
      innerHeight: win.innerHeight,
      scale: win.visualViewport?.scale ?? null,
      screenWidth: win.screen.width,
      screenHeight: win.screen.height,
      landscape: landscape.matches,
    });
    doc.documentElement.style.setProperty("--short-fix", `${fix}px`);
  };
  set();
  win.addEventListener("resize", set);
  win.addEventListener("orientationchange", set);
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
