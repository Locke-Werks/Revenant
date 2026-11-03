// The running app: the link to the gateway, the view, and the waterfall it
// drives. Receivers (Task 16) hang off the handle this returns.

import { attachGestures } from "./gestures";
import { Link, socketUrl, wireLifecycle, type KeyStore, type SocketLike } from "./link";
import { SpectrumTrace } from "./spectrum_trace";
import { clampView, MAX_WIDTH_PX, panBy, pxToHz, sameView, ViewSync, zoomAt, type Band, type View } from "./view";
import { createWaterfall } from "./waterfall";

export function appScreenHtml(): string {
  return `
<main class="wf">
  <canvas id="trace" class="wf-trace"></canvas>
  <div id="wf-body" class="wf-body"><canvas id="waterfall" class="wf-canvas"></canvas></div>
</main>`;
}

export interface AppHandle {
  link: Link;
  view(): View | null;
  // Replaced by the receiver UI; logging until then.
  onTap: (hz: number) => void;
  onLongPress: (hz: number) => void;
}

// Private browsing and a full disk both make localStorage throw; a resume
// token held in memory still covers drops while the app stays open.
function keyStore(): KeyStore {
  try {
    const s = window.localStorage;
    s.setItem("revenant.probe", "1");
    s.removeItem("revenant.probe");
    return s;
  } catch {
    const m = new Map<string, string>();
    return {
      getItem: (k) => m.get(k) ?? null,
      setItem: (k, v) => void m.set(k, v),
      removeItem: (k) => void m.delete(k),
    };
  }
}

export function mountApp(root: HTMLElement): AppHandle {
  const traceCanvas = root.querySelector<HTMLCanvasElement>("#trace")!;
  const body = root.querySelector<HTMLElement>("#wf-body")!;
  const wfCanvas = root.querySelector<HTMLCanvasElement>("#waterfall")!;

  const link = new Link({
    url: socketUrl(location),
    socket: (url) => new WebSocket(url) as unknown as SocketLike,
    timers: { setTimeout: (fn, ms) => window.setTimeout(fn, ms), clearTimeout: (h) => window.clearTimeout(h as number) },
    store: keyStore(),
    now: () => performance.now(),
  });
  const waterfall = createWaterfall(wfCanvas);
  const trace = new SpectrumTrace(traceCanvas);

  let band: Band | null = null;
  let view: View | null = null;
  let widthDev = 1;
  let drawQueued = false;

  const draw = () => {
    drawQueued = false;
    waterfall.draw();
    trace.draw();
  };
  const requestDraw = () => {
    if (drawQueued) return;
    drawQueued = true;
    requestAnimationFrame(draw);
  };

  const sync = new ViewSync(
    (v) => {
      if (!link.ready) return;
      link.request("setView", { centerHz: v.centerHz, spanHz: v.spanHz, widthPx: v.widthPx }).catch((e: unknown) => {
        console.warn("setView", e);
      });
    },
    (fn) => requestAnimationFrame(fn),
  );

  const apply = (next: View) => {
    const v = band ? clampView(next, band) : next;
    if (sameView(v, view)) return;
    view = v;
    waterfall.setView(v);
    trace.setView(v);
    sync.update(v);
    requestDraw();
  };

  const resize = () => {
    const dpr = window.devicePixelRatio || 1;
    const wCss = body.clientWidth;
    const hCss = body.clientHeight;
    widthDev = Math.max(1, Math.min(MAX_WIDTH_PX, Math.round(wCss * dpr)));
    waterfall.resize(Math.max(1, Math.round(wCss * dpr)), Math.max(1, Math.round(hCss * dpr)));
    trace.resize(Math.max(1, Math.round(traceCanvas.clientWidth * dpr)), Math.max(1, Math.round(traceCanvas.clientHeight * dpr)));
    if (view) apply({ ...view, widthPx: widthDev });
    requestDraw();
  };
  // Rotation, the URL-less standalone chrome settling, and split view all come
  // through as a size change.
  new ResizeObserver(resize).observe(body);
  resize();
  // Moving between displays or a zoom change alters devicePixelRatio without
  // resizing the element. The query matches only the current ratio, so it is
  // re-armed for the new one each time it fires.
  const watchDpr = () => {
    const mq = window.matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
    mq.addEventListener("change", () => {
      resize();
      watchDpr();
    }, { once: true });
  };
  watchDpr();

  const handle: AppHandle = {
    link,
    view: () => view,
    onTap: (hz) => console.log("tap", Math.round(hz)),
    onLongPress: (hz) => console.log("long press", Math.round(hz)),
  };

  // Gestures bind to the wrapper, which outlives a canvas swapped for the 2D
  // fallback. Coordinates are CSS pixels, so the view is scaled to CSS width.
  const hzAt = (x: number) => (view ? pxToHz({ ...view, widthPx: body.clientWidth || 1 }, x) : Number.NaN);
  attachGestures(
    body,
    {
      pan: (dHz) => {
        if (view && Number.isFinite(dHz)) apply(panBy(view, dHz));
      },
      zoom: (factor, anchorHz) => {
        if (view && Number.isFinite(anchorHz)) apply(zoomAt(view, factor, anchorHz));
      },
      tap: (hz) => {
        if (Number.isFinite(hz)) handle.onTap(hz);
      },
      longPress: (hz) => {
        if (Number.isFinite(hz)) handle.onLongPress(hz);
      },
    },
    hzAt,
  );

  link.on("source", (e) => {
    if (e.open !== true || typeof e.centerHz !== "number" || typeof e.rateHz !== "number") {
      band = null;
      return;
    }
    const changed = !band || band.centerHz !== e.centerHz || band.rateHz !== e.rateHz;
    band = { centerHz: e.centerHz, rateHz: e.rateHz };
    if (!changed && view) return;
    // A retuned or reopened source: history was cut for another band.
    waterfall.clear();
    apply({ centerHz: band.centerHz, spanHz: band.rateHz, widthPx: widthDev });
  });
  link.on("ready", () => {
    // The gateway may hold no view for this socket, or a resumed one from
    // before a rotation; send ours either way.
    sync.forget();
    if (view) sync.update(view);
  });
  link.on("spectrum", (f) => {
    // A resumed session already had its source event before this page
    // existed (a relaunch from the home screen), so the first row's own view
    // stands in until a source event names the band.
    if (!view) apply({ centerHz: f.centerHz, spanHz: f.spanHz, widthPx: widthDev });
    waterfall.pushRow(f);
    trace.setRow(f);
    requestDraw();
  });

  wireLifecycle(link, document, window);
  link.start();
  return handle;
}
