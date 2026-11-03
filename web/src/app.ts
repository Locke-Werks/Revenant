// The running app: the link to the gateway, the view, and the waterfall it
// drives, the receivers and their controls, and the status the phone shows.

import { HlsPlayer, silentWavUrl } from "./audio/hls_player";
import { browserMediaSession, KeepListening } from "./audio/keep_listening";
import { browserLiveAudioDeps, LiveAudio } from "./audio/live";
import { parseDetections, snapTap, type Detection } from "./detections";
import { attachGestures } from "./gestures";
import { TuneLimiter } from "./tune_limiter";
import { Link, socketUrl, wireLifecycle, type KeyStore, type SocketLike } from "./link";
import { SpectrumTrace } from "./spectrum_trace";
import { clampView, MAX_WIDTH_PX, panBy, pxToHz, sameView, ViewSync, zoomAt, type Band, type View } from "./view";
import { createWaterfall, RING_ROWS } from "./waterfall";
import type { SourceEvent } from "./protocol";
import { captionFromEvent, CaptionLayer, CaptionStore, RowClock } from "./ui/captions";
import { decodedEntry, FeedModel, FeedView, transcriptEntry } from "./ui/feed";
import { RadioPanel } from "./ui/radio_panel";
import { formatMHz, ReceiverController, ReplaceGate } from "./ui/receivers";
import { Sheet } from "./ui/sheet";
import { SpeechModel, speechLine, SpeechSwitch } from "./ui/speech_switch";
import { StatusModel } from "./ui/status";
import { Notes, Toasts } from "./ui/toast";
import { levelInBand, TopBar } from "./ui/top_bar";

export function appScreenHtml(): string {
  return `
<main class="app">
  <div class="top" id="top-slot"></div>
  <div class="stage" id="stage">
    <canvas id="trace" class="wf-trace"></canvas>
    <div id="wf-body" class="wf-body"><canvas id="waterfall" class="wf-canvas"></canvas><div id="captions" class="captions" aria-hidden="true"></div></div>
    <div class="overlay">
      <div class="status-chips" id="status-chips"></div>
      <div class="pill" id="status-pill" role="status" hidden></div>
      <div class="behind" id="kl-behind" hidden>Background audio, about 3 s behind</div>
      <button type="button" class="golive" id="kl-golive" hidden>Go live</button>
      <div class="notes" id="notes" role="status"></div>
      <div class="toasts" id="toasts" role="status" aria-live="polite"></div>
    </div>
  </div>
  <nav class="rail" aria-label="Receivers">
    <div class="chips" id="chips"></div>
    <button type="button" class="btn controls" id="open-sheet" aria-expanded="false" aria-controls="sheet">Controls</button>
  </nav>
  <div id="sheet-slot"></div>
  <audio id="kl-audio" playsinline preload="none"></audio>
</main>`;
}

export interface AppHandle {
  link: Link;
  audio: LiveAudio;
  keep: KeepListening;
  receivers: ReceiverController;
  view(): View | null;
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

  // Captions ride on the same rows as the waterfall: the clock keeps each
  // row's source_start in the waterfall's own ring order.
  const clock = new RowClock();
  const captionStore = new CaptionStore();
  const captionLayer = new CaptionLayer(root.querySelector<HTMLElement>("#captions")!, captionStore, clock);
  let rendererState = waterfall.state;
  const forgetRows = () => {
    clock.clear();
    captionStore.clear();
  };

  let band: Band | null = null;
  let view: View | null = null;
  let widthDev = 1;
  let drawQueued = false;

  const draw = () => {
    drawQueued = false;
    waterfall.draw();
    trace.draw();
    layoutCaptions();
  };
  const layoutCaptions = () => {
    if (!view) return;
    const heightCss = body.clientHeight;
    const widthCss = body.clientWidth;
    if (!(heightCss > 0) || !(widthCss > 0)) return;
    // The GL ring is stretched over the whole height; the 2D fallback
    // scrolls a fixed number of device pixels per row.
    const twoD = waterfall.state === "2d";
    const rowHeightCss = twoD ? 2 / (window.devicePixelRatio || 1) : heightCss / RING_ROWS;
    const visibleRows = twoD ? Math.max(1, Math.floor(heightCss / rowHeightCss)) : RING_ROWS;
    captionLayer.layout({ view, widthCss, rowHeightCss, visibleRows });
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

  const stage = root.querySelector<HTMLElement>("#stage")!;
  const toasts = new Toasts(root.querySelector<HTMLElement>("#toasts")!);
  const notes = new Notes(root.querySelector<HTMLElement>("#notes")!);
  const toast = (text: string) => toasts.show(text);

  // LiveAudio, keep listening and the receivers all see vrxRemoved through the
  // gate, which holds a replaced id's removal until the reply has moved them.
  const gate = new ReplaceGate(link);
  const audio = new LiveAudio(browserLiveAudioDeps(gate));

  const hlsMedia = root.querySelector<HTMLAudioElement>("#kl-audio")!;
  const hls = new HlsPlayer({
    media: hlsMedia,
    probe: (url) => fetch(url, { cache: "no-store" }).then((r) => r.ok),
    setTimeout: (fn, ms) => window.setTimeout(fn, ms),
    clearTimeout: (h) => window.clearTimeout(h as number),
    silence: silentWavUrl(),
  });
  const behind = root.querySelector<HTMLElement>("#kl-behind")!;
  const goLive = root.querySelector<HTMLButtonElement>("#kl-golive")!;
  let sheet: Sheet | null = null;
  const keep = new KeepListening({
    link: gate,
    live: audio,
    hls,
    ui: {
      behind: (show) => void (behind.hidden = !show),
      goLive: (show) => void (goLive.hidden = !show),
      message: (text) => notes.set("keep", text),
      keep: (on) => sheet?.keepListening(on),
    },
    media: browserMediaSession(),
    hidden: () => document.hidden,
    setTimeout: (fn, ms) => window.setTimeout(fn, ms),
    clearTimeout: (h) => window.clearTimeout(h as number),
  });
  // The pill resumes the AudioContext, which needs the tap itself.
  goLive.addEventListener("click", () => keep.goLive());
  document.addEventListener("visibilitychange", () => keep.visibility(document.hidden));

  const receivers = new ReceiverController({
    link: gate,
    audio,
    keep,
    toast,
    audioNote: (text) => notes.set("audio", text),
  });

  const openSheetBtn = root.querySelector<HTMLButtonElement>("#open-sheet")!;
  sheet = new Sheet({
    setMode: (m) => receivers.setMode(m),
    setBandwidth: (hz) => receivers.setBandwidth(hz),
    setSquelch: (db) => receivers.setSquelch(db),
    setTranscribe: (c) => receivers.setTranscribe(c),
    remove: (vrx) => receivers.remove(vrx),
    // The HLS element needs this gesture to play.
    toggleKeepListening: () => {
      if (keep.handoff.keepListening) keep.off();
      else keep.on();
    },
  });
  root.querySelector("#sheet-slot")!.append(sheet.el);
  const radio = new RadioPanel({ link: gate, toast });
  sheet.extra.append(radio.el);

  // ---- Speech, transcripts and the feed -------------------------------------
  const speechModel = new SpeechModel({ link: gate });
  const feedModel = new FeedModel();
  const feedPanel = document.createElement("section");
  feedPanel.className = "panel feed-panel";
  const feedHead = document.createElement("h2");
  feedHead.textContent = "Decoded and spoken";
  const feedHost = document.createElement("div");
  feedHost.className = "feed";
  feedPanel.append(feedHead, feedHost);
  sheet.extra.prepend(feedPanel);
  const feedView = new FeedView(feedHost, feedModel, () => document.createElement("div"));
  let feedQueued = false;
  const queueFeed = () => {
    if (!sheet!.open || feedQueued) return;
    feedQueued = true;
    requestAnimationFrame(() => {
      feedQueued = false;
      feedView.render(receivers.list.active);
    });
  };
  feedModel.onChange(queueFeed);
  receivers.list.onChange(queueFeed);
  sheet.onOpen((open) => {
    if (open) feedView.render(receivers.list.active);
  });
  receivers.onRemap((pairs) => {
    captionStore.remap(pairs);
    feedModel.remap(pairs);
  });
  receivers.onGone((vrx) => {
    captionStore.dropReceiver(vrx);
    feedModel.dropReceiver(vrx);
  });
  speechModel.onChange(() => {
    sheet!.speechLine(speechLine(speechModel.status));
    const s = speechModel.status;
    notes.set("speech", s?.enabled === true && s.modelState === "failed" ? speechLine(s) : null);
  });
  sheet.speechLine(speechLine(null));
  link.on("transcript", (e) => {
    const c = captionFromEvent(e);
    if (c) {
      captionStore.add(c);
      requestDraw();
    }
    const f = transcriptEntry(e);
    if (f) feedModel.add(f);
  });
  link.on("decoded", (e) => {
    const f = decodedEntry(e);
    if (f) feedModel.add(f);
  });
  sheet.onOpen((open) => openSheetBtn.setAttribute("aria-expanded", String(open)));
  openSheetBtn.addEventListener("click", () => sheet!.setOpen(!sheet!.open));

  const top = new TopBar({
    tune: (hz) => receivers.tap(hz),
    openSheet: () => sheet!.setOpen(true),
    toast,
  });
  top.el.append(new SpeechSwitch(speechModel, toast).el);
  root.querySelector("#top-slot")!.append(top.el);

  const chipsEl = root.querySelector<HTMLElement>("#chips")!;
  const renderReceivers = () => {
    const list = receivers.list.list();
    const active = receivers.list.active;
    chipsEl.textContent = "";
    for (const r of list) {
      const b = document.createElement("button");
      b.type = "button";
      b.className = "chip rx";
      b.setAttribute("aria-pressed", String(r.vrx === active));
      const flag = r.audioError ? " !" : r.statusUnavailable ? " ?" : "";
      b.textContent = `${formatMHz(r.hz)} ${r.mode.toUpperCase()}${flag}`;
      const note = r.audioError ?? (r.statusUnavailable ? "the gateway could not read this receiver back; shown as sent" : "");
      b.setAttribute("aria-label", `Receiver ${formatMHz(r.hz)} MHz ${r.mode.toUpperCase()}${note ? `, ${note}` : ""}`);
      if (note) b.title = note;
      // A tap in its own handler, so live audio may start from it.
      b.addEventListener("click", () => receivers.select(r.vrx));
      chipsEl.append(b);
    }
    const current = active === null ? null : receivers.list.get(active) ?? null;
    top.show(current);
    sheet!.show(current, list.length > 0);
  };
  receivers.list.onChange(renderReceivers);
  renderReceivers();

  // ---- Status --------------------------------------------------------------
  const status = new StatusModel();
  const pill = root.querySelector<HTMLElement>("#status-pill")!;
  const statusChips = root.querySelector<HTMLElement>("#status-chips")!;
  const renderStatus = () => {
    const text = status.pill();
    pill.hidden = text === null;
    pill.textContent = text ?? "";
    stage.classList.toggle("frozen", status.frozen());
    statusChips.textContent = "";
    for (const c of status.chips()) {
      const d = document.createElement("div");
      d.className = "status-chip";
      d.textContent = c;
      statusChips.append(d);
    }
  };
  renderStatus();
  link.on("ready", (e) => {
    status.ready(e.resumed);
    renderStatus();
  });
  link.on("down", () => {
    status.socket(false);
    renderStatus();
  });
  link.on("link", (e) => {
    status.link(e.state, e.detail, e.recreated !== undefined || e.lost !== undefined);
    renderStatus();
  });
  link.on("vrxEventsEnded", (e) => {
    status.eventsEnded(typeof e.reason === "string" ? e.reason : "ended");
    renderStatus();
  });

  // ---- Gestures ------------------------------------------------------------
  // Gestures bind to the wrapper, which outlives a canvas swapped for the 2D
  // fallback. Coordinates are CSS pixels, so the view is scaled to CSS width.
  const hzAt = (x: number) => (view ? pxToHz({ ...view, widthPx: body.clientWidth || 1 }, x) : Number.NaN);
  // Latest list from the engine; empty until the first event.
  let detections: Detection[] = [];
  link.on("detections", (e) => {
    detections = parseDetections(e);
  });

  const tuneLimiter = new TuneLimiter(
    (hz) => receivers.tuneDragged(hz),
    () => performance.now(),
    (fn, ms) => {
      const h = window.setTimeout(fn, ms);
      return () => window.clearTimeout(h);
    },
  );
  // Where the finger has taken the receiver; null between drags.
  let dragHz: number | null = null;
  // The view follows only once the receiver is this close to an edge, so a
  // small nudge does not slide the picture under the finger.
  const EDGE_FRACTION = 0.1;

  attachGestures(
    body,
    {
      dragTune: (dHz) => {
        if (!Number.isFinite(dHz)) return;
        let next = dragHz;
        if (next === null) {
          const id = receivers.list.active;
          const r = id === null ? undefined : receivers.list.get(id);
          if (!r) return;
          next = r.hz;
        }
        next = Math.max(1, next + dHz);
        // The marker and chip move now; the engine confirms through status.
        // Only a drag that took effect accumulates.
        if (!receivers.preview(next)) return;
        dragHz = next;
        tuneLimiter.set(dragHz);
        if (view) {
          const lo = view.centerHz - view.spanHz / 2;
          const hi = view.centerHz + view.spanHz / 2;
          const margin = view.spanHz * EDGE_FRACTION;
          if (dragHz < lo + margin) apply(panBy(view, dragHz - (lo + margin)));
          else if (dragHz > hi - margin) apply(panBy(view, dragHz - (hi - margin)));
        }
        requestDraw();
      },
      dragEnd: () => {
        dragHz = null;
        tuneLimiter.flush();
        receivers.endDrag();
      },
      pan: (dHz) => {
        if (view && Number.isFinite(dHz)) apply(panBy(view, dHz));
      },
      zoom: (factor, anchorHz) => {
        if (view && Number.isFinite(anchorHz)) apply(zoomAt(view, factor, anchorHz));
      },
      tap: (hz) => {
        if (Number.isFinite(hz)) receivers.tap(snapTap(hz, detections));
      },
      longPress: (hz) => {
        if (Number.isFinite(hz)) receivers.longPress(hz);
      },
    },
    hzAt,
  );

  let sourceEpoch: number | undefined;
  link.on("source", (raw) => {
    const e = raw as unknown as SourceEvent;
    // Another source counts samples from its own zero: captions and rows
    // from before it must not be matched against the new clock.
    if (e.open !== true || (sourceEpoch !== undefined && e.epoch !== sourceEpoch)) forgetRows();
    sourceEpoch = e.epoch;
    status.source(e);
    radio.source(e);
    renderStatus();
    if (e.open !== true || typeof e.centerHz !== "number" || typeof e.rateHz !== "number") {
      band = null;
      return;
    }
    const changed = !band || band.centerHz !== e.centerHz || band.rateHz !== e.rateHz;
    band = { centerHz: e.centerHz, rateHz: e.rateHz };
    if (!changed && view) return;
    // A retuned or reopened source: history was cut for another band.
    waterfall.clear();
    forgetRows();
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
    // Frozen means the engine is gone: whatever trickles in is not news.
    if (status.frozen()) return;
    // A context loss or the 2D handover drops history, and the rows it kept
    // are not the ones the clock holds.
    if (waterfall.state !== rendererState) {
      rendererState = waterfall.state;
      forgetRows();
    }
    if (waterfall.state !== "lost" && clock.push(Number(f.sourceStart))) captionStore.clear();
    waterfall.pushRow(f);
    trace.setRow(f);
    const active = receivers.list.active;
    const r = active === null ? undefined : receivers.list.get(active);
    if (r) {
      const half = (r.bandwidthHz ?? 12_500) / 2;
      top.level(levelInBand(f, r.hz - half, r.hz + half));
    }
    requestDraw();
  });

  wireLifecycle(link, document, window);
  link.start();
  return { link, audio, keep, receivers, view: () => view };
}
