// The running app: the link to the gateway, the view, and the waterfall it
// drives, the receivers and their controls, and the status the phone shows.

import { HlsPlayer, silentWavUrl } from "./audio/hls_player";
import { browserMediaSession, KeepListening } from "./audio/keep_listening";
import { browserLiveAudioDeps, LiveAudio } from "./audio/live";
import { labelAt, labelText, parseDetections, snapTap, type Detection } from "./detections";
import { encryptedFlag, modeLabel, parseCatalogue, voiceStatus, type DecoderCatalogue } from "./decoders";
import { attachGestures } from "./gestures";
import { TuneLimiter } from "./tune_limiter";
import { Link, socketUrl, wireLifecycle, type KeyStore, type SocketLike } from "./link";
import { parseDisplay } from "./display";
import { DEFAULT_ACCENT, DETECTION_COLOR, detectionBoxes, passbandHz } from "./markers";
import { drawReceivers, type ReceiverMark } from "./overlay";
import { RowPacer } from "./smooth";
import { SpectrumTrace } from "./spectrum_trace";
import { clampView, viewBand, MAX_WIDTH_PX, panBy, pxToHz, sameView, ViewSync, zoomAt, type Band, type View } from "./view";
import { boxCarryRows, createWaterfall, RING_ROWS } from "./waterfall";
import type { SourceEvent } from "./protocol";
import { captionFromEvent, CaptionLayer, CaptionStore, RowClock } from "./ui/captions";
import { decodedEntry, FeedModel, FeedView, transcriptEntry } from "./ui/feed";
import { RadioPanel } from "./ui/radio_panel";
import { formatMHz, ReceiverController, ReplaceGate } from "./ui/receivers";
import { Sheet, SHEET_TABS, TAB_LABELS, type SheetTab } from "./ui/sheet";
import { readConfidence, SignalsPanel } from "./ui/signals";
import { SpeechModel, speechLine, SpeechSwitch } from "./ui/speech_switch";
import { StatusModel } from "./ui/status";
import { Notes, Toasts } from "./ui/toast";
import { ChipPress } from "./ui/chip_press";
import { DialStrip } from "./ui/dial_strip";
import { Ruler } from "./ui/ruler";
import { levelInBand, TopBar } from "./ui/top_bar";
import { CentreTuner } from "./centre_tuner";
import { clampToRanges } from "./band_edge";

export function appScreenHtml(): string {
  return `
<main class="app">
  <div class="top" id="top-slot"></div>
  <div class="stage" id="stage">
    <canvas id="trace" class="wf-trace"></canvas>
    <canvas id="ruler" class="wf-ruler" aria-hidden="true"></canvas>
    <div id="wf-body" class="wf-body"><canvas id="waterfall" class="wf-canvas"></canvas><canvas id="wf-marks" class="wf-marks" aria-hidden="true"></canvas><div id="captions" class="captions" aria-hidden="true"></div></div>
    <div class="overlay">
      <div class="status-chips" id="status-chips" role="status"></div>
      <div class="behind" id="kl-behind" hidden>Background audio, about 3 s behind</div>
      <button type="button" class="golive" id="kl-golive" hidden>Go live</button>
      <div class="notes" id="notes" role="status"></div>
      <div class="toasts" id="toasts" role="status" aria-live="polite"></div>
    </div>
  </div>
  <nav class="rail" id="rail" aria-label="Receivers" hidden>
    <div class="chips" id="chips"></div>
  </nav>
  <div class="dial" id="dial-slot"></div>
  <nav class="grabber" aria-label="Controls">
    <span class="handle" aria-hidden="true"></span>
    <div class="grab-tabs" id="grab-tabs"></div>
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
  // One accent for every marker, read once from the stylesheet so the visual
  // pass can change it in CSS alone.
  const accent = getComputedStyle(document.documentElement).getPropertyValue("--accent").trim() || DEFAULT_ACCENT;
  const waterfall = createWaterfall(wfCanvas, DETECTION_COLOR);
  const trace = new SpectrumTrace(traceCanvas, accent);
  const rulerCanvas = root.querySelector<HTMLCanvasElement>("#ruler")!;
  const ruler = new Ruler(rulerCanvas);
  const marksCanvas = root.querySelector<HTMLCanvasElement>("#wf-marks")!;
  const marks = marksCanvas.getContext("2d");

  // Captions ride on the same rows as the waterfall: the clock keeps each
  // row's source_start in the waterfall's own ring order.
  const clock = new RowClock();
  const captionStore = new CaptionStore();
  const captionLayer = new CaptionLayer(root.querySelector<HTMLElement>("#captions")!, captionStore, clock);
  let rendererState = waterfall.state;
  // Rows arrive at the engine's rate and frames at the display's; the pacer
  // says how far the history has scrolled between them.
  const pacer = new RowPacer();
  const forgetRows = () => {
    clock.clear();
    captionStore.clear();
    pacer.clear();
  };

  let band: Band | null = null;
  // Where a centre tune with no receiver is taking the radio; the dimmed
  // readout shows it instead of the view's centre while it lasts.
  let centrePreviewHz: number | null = null;
  // The band trimmed to the spectrum's real edges, as the source event gives
  // them; what the view is clamped to.
  let limits: Band | null = null;
  let view: View | null = null;
  let widthDev = 1;
  let drawQueued = false;

  const draw = (now: number) => {
    drawQueued = false;
    // The 2D fallback scrolls whole rows of pixels it cannot move again, so
    // it steps; everything over it then sits at rest too.
    const scrolling = waterfall.scrolls;
    const shift = scrolling ? pacer.phase(now) - 1 : 0;
    waterfall.draw(shift);
    trace.draw(now);
    ruler.draw();
    top.viewCentre(centrePreviewHz ?? band?.centerHz ?? (view ? view.centerHz : null));
    drawMarks(shift);
    layoutCaptions(shift);
    // Another frame only while something is moving: a still screen with no
    // rows arriving costs nothing.
    if ((scrolling && pacer.moving(now)) || trace.moving()) requestDraw();
  };
  const drawMarks = (shift: number) => {
    if (!marks) return;
    marks.clearRect(0, 0, marksCanvas.width, marksCanvas.height);
    if (!view) return;
    waterfall.drawBoxes(marks, DETECTION_COLOR, shift);
    drawReceivers(marks, view, receiverMarks, activeMark, accent);
  };
  // Filled in once the receiver controller exists; drawn on the current view.
  let receiverMarks: ReceiverMark[] = [];
  let activeMark: number | null = null;
  const layoutCaptions = (shift: number) => {
    if (!view) return;
    const heightCss = body.clientHeight;
    const widthCss = body.clientWidth;
    if (!(heightCss > 0) || !(widthCss > 0)) return;
    // The GL ring is stretched over the whole height; the 2D fallback
    // scrolls a fixed number of device pixels per row.
    const twoD = waterfall.state === "2d";
    const rowHeightCss = twoD ? 2 / (window.devicePixelRatio || 1) : heightCss / RING_ROWS;
    const visibleRows = twoD ? Math.max(1, Math.floor(heightCss / rowHeightCss)) : RING_ROWS;
    captionLayer.layout({ view, widthCss, rowHeightCss, visibleRows, offsetRows: shift });
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
    const v = limits ? clampView(next, limits) : next;
    if (sameView(v, view)) return;
    view = v;
    waterfall.setView(v);
    trace.setView(v);
    ruler.setView(v);
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
    ruler.resize(Math.max(1, Math.round(rulerCanvas.clientWidth * dpr)), Math.max(1, Math.round(rulerCanvas.clientHeight * dpr)));
    marksCanvas.width = Math.max(1, Math.round(wCss * dpr));
    marksCanvas.height = Math.max(1, Math.round(hCss * dpr));
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
  const toast = (text: string, key?: string) => toasts.show(text, key);

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

  // The engine's decoders, asked for on every connect because an engine
  // restart can bring a different set of vocoder plugins.
  let catalogue: DecoderCatalogue | null = null;
  // When each receiver last decoded an encrypted call. A call's flags repeat
  // every superframe, so one this old has ended or gone clear.
  const ENCRYPTED_HOLD_MS = 5_000;
  const encryptedAt = new Map<number, number>();
  // Detections as the receiver chips read them; the full list lives below.
  let chipDetections: Detection[] = [];

  const receivers = new ReceiverController({
    link: gate,
    audio,
    keep,
    toast,
    audioNote: (text) => notes.set("audio", text),
    tuneRanges: () => radio.tuneRanges(),
    decoders: () => catalogue?.decoders ?? null,
  });

  sheet = new Sheet({
    setMode: (m) => receivers.setMode(m),
    setBandwidth: (hz) => receivers.setBandwidth(hz),
    setSquelch: (db) => receivers.setSquelch(db),
    setTranscribe: (c) => receivers.setTranscribe(c),
    setDecoders: (names) => receivers.setDecoders(names),
    remove: (vrx) => receivers.remove(vrx),
    // The HLS element needs this gesture to play.
    toggleKeepListening: () => {
      if (keep.handoff.keepListening) keep.off();
      else keep.on();
    },
  });
  root.querySelector("#sheet-slot")!.append(sheet.el);
  const radio = new RadioPanel({ link: gate, toast });
  sheet.radioHost.append(radio.el);

  // ---- Signals tab ---------------------------------------------------------
  // Newest source sample index from the spectrum rows: lastSeen is an index
  // in the same count, so this is "now" for the age column.
  let newestIndex: number | null = null;
  let sheetTab: SheetTab = "tune";
  const prefs = keyStore();
  const CONFIDENCE_KEY = "revenant.minConfidence";
  const sendConfidence = (v: number) => {
    link.request("setDetection", { minConfidence: v }).catch((e: unknown) => toast(e instanceof Error ? e.message : String(e)));
  };
  const signals = new SignalsPanel({
    hasActiveReceiver: () => receivers.list.active !== null,
    stopDial: () => dial.stop(),
    tuneFit: (hz, bw) => receivers.tuneFit(hz, bw),
    reveal: (hz) => {
      if (!view) return;
      const lo = view.centerHz - view.spanHz / 2;
      const hi = view.centerHz + view.spanHz / 2;
      if (hz < lo || hz > hi) apply(panBy(view, hz - view.centerHz));
    },
    toast,
    setMinConfidence: sendConfidence,
    initialConfidence: readConfidence(prefs.getItem(CONFIDENCE_KEY)),
    saveConfidence: (v) => {
      try {
        prefs.setItem(CONFIDENCE_KEY, String(v));
      } catch {
        // A full disk costs the preference, not the session.
      }
    },
    clock: () => (newestIndex !== null && band ? { index: newestIndex, rateHz: band.rateHz } : null),
  });
  sheet.signalsHost.append(signals.el);
  const syncSignalsVisible = () => signals.setVisible(sheet!.open && sheetTab === "signals");
  sheet.onTab((tab) => {
    sheetTab = tab;
    syncSignalsVisible();
  });
  sheet.onOpen(syncSignalsVisible);
  // The gateway keeps the bar per session and re-applies its own copy on a
  // resume, which can disagree with what this relaunched page shows. The
  // slider is the phone's record, so it is sent on every connect.
  link.on("ready", () => sendConfidence(signals.value()));

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
  let encryptedTimer: number | undefined;
  link.on("decoded", (e) => {
    const f = decodedEntry(e);
    if (f) feedModel.add(f);
    const vrx = typeof e.vrx === "number" ? e.vrx : null;
    const flag = encryptedFlag(e);
    if (vrx === null || flag === undefined) return;
    const was = encryptedAt.has(vrx);
    if (flag) encryptedAt.set(vrx, Date.now());
    else encryptedAt.delete(vrx);
    if (was === flag && !flag) return;
    // Redrawn once the hold runs out, so the chip clears without a message.
    window.clearTimeout(encryptedTimer);
    encryptedTimer = window.setTimeout(() => renderReceivers(), ENCRYPTED_HOLD_MS + 50);
    if (was !== flag) renderReceivers();
  });
  link.on("ready", () => {
    gate.request("listDecoders").then(
      (reply) => {
        catalogue = parseCatalogue(reply);
        sheet!.setCatalogue(catalogue.decoders);
        renderReceivers();
      },
      // An engine that cannot list its decoders leaves the section empty;
      // DV receivers still get their own decoder from the gateway.
      () => undefined,
    );
  });
  // The grabber names the sheet's tabs: a tap opens that tab, and a tap on
  // the tab already showing closes the sheet.
  const grabTabs = root.querySelector<HTMLElement>("#grab-tabs")!;
  const grabButtons = SHEET_TABS.map((t) => {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "grab-tab";
    b.textContent = TAB_LABELS[t];
    b.setAttribute("aria-controls", "sheet");
    b.addEventListener("click", () => {
      if (sheet!.open && sheetTab === t) {
        sheet!.setOpen(false);
        return;
      }
      sheet!.selectTab(t);
      sheet!.setOpen(true);
    });
    grabTabs.append(b);
    return { t, b };
  });
  const syncGrabber = () => {
    for (const { t, b } of grabButtons) b.setAttribute("aria-expanded", String(sheet!.open && sheetTab === t));
  };
  sheet.onOpen(syncGrabber);
  sheet.onTab(syncGrabber);
  syncGrabber();

  // The dial shares the waterfall drag's preview-and-hold path, so the chip
  // and readout follow it and a stale reply cannot pull it back.
  // With no receiver the dial moves the radio's centre: a pseudo receiver
  // with a negative id, which the dial stops on as soon as a real one is
  // active.
  const CENTRE_VRX = -1;
  const centre = new CentreTuner({
    send: (hz) =>
      gate.request("setCenter", { hz }).catch((e: unknown) => toast(e instanceof Error ? e.message : String(e), "tune")),
    ranges: () => radio.tuneRanges(),
    preview: (hz) => showCentre(hz),
  });
  const showCentre = (hz: number | null) => {
    centrePreviewHz = hz;
    top.viewCentre(hz ?? band?.centerHz ?? (view ? view.centerHz : null));
  };
  const centreLimiter = new TuneLimiter(
    (hz) => centre.set(hz),
    () => performance.now(),
    (fn, ms) => {
      const h = window.setTimeout(fn, ms);
      return () => window.clearTimeout(h);
    },
  );
  const dial = new DialStrip({
    active: () => {
      const id = receivers.list.active;
      const r = id === null ? undefined : receivers.list.get(id);
      if (r) return { vrx: r.vrx, hz: r.hz, mode: r.mode };
      if (!band) return null;
      return { vrx: CENTRE_VRX, hz: centrePreviewHz ?? band.centerHz, mode: "centre" };
    },
    preview: (hz) => {
      if (receivers.list.active !== null) return receivers.preview(hz) !== null;
      if (!band) return false;
      showCentre(Math.round(hz));
      return true;
    },
    tune: (vrx, hz) => (vrx === CENTRE_VRX ? centre.set(hz) : receivers.tuneDragged(hz, vrx)),
    end: () => receivers.endDrag(),
    now: () => performance.now(),
    schedule: (fn, ms) => {
      const h = window.setTimeout(fn, ms);
      return () => window.clearTimeout(h);
    },
    frame: (fn) => {
      const h = requestAnimationFrame(fn);
      return () => cancelAnimationFrame(h);
    },
  });
  root.querySelector("#dial-slot")!.append(dial.el);

  // A digit swipe previews and sends like a waterfall drag, so the readout
  // follows the finger and the gateway sees a rate-limited stream.
  let swiping = false;
  let swipingCentre = false;
  const digitLimiter = new TuneLimiter(
    (hz) => receivers.tuneDragged(hz),
    () => performance.now(),
    (fn, ms) => {
      const h = window.setTimeout(fn, ms);
      return () => window.clearTimeout(h);
    },
  );
  const top = new TopBar({
    tune: (hz) => {
      dial.stop();
      receivers.tap(hz);
    },
    // A waterfall drag in progress owns the receiver, so the swipe is ignored;
    // the reverse is guarded in dragTune.
    swipeBegin: () => {
      if (dragHz !== null) return null;
      dial.stop();
      swiping = true;
      const id = receivers.list.active;
      const r = id === null ? undefined : receivers.list.get(id);
      if (r) return r.hz;
      // The dimmed readout: the swipe moves the radio's centre.
      swipingCentre = band !== null;
      return band ? centrePreviewHz ?? band.centerHz : null;
    },
    swipeStep: (hz) => {
      if (swipingCentre) {
        showCentre(Math.round(hz));
        centreLimiter.set(hz);
        return;
      }
      if (!receivers.preview(hz)) return;
      digitLimiter.set(hz);
    },
    swipeEnd: () => {
      swiping = false;
      if (swipingCentre) {
        swipingCentre = false;
        centreLimiter.flush();
        return;
      }
      digitLimiter.flush();
      receivers.endDrag();
    },
    openSheet: () => sheet!.setOpen(true),
    toast,
  });
  top.el.append(new SpeechSwitch(speechModel, toast).el);
  root.querySelector("#top-slot")!.append(top.el);

  const chipsEl = root.querySelector<HTMLElement>("#chips")!;
  const railEl = root.querySelector<HTMLElement>("#rail")!;
  // Removal offered on the chip itself, anchored under it. It lives in the
  // rail, not among the chips, because the chips are rebuilt on every change.
  const confirmEl = document.createElement("div");
  confirmEl.className = "chip-confirm";
  confirmEl.setAttribute("role", "alertdialog");
  confirmEl.hidden = true;
  confirmEl.innerHTML = `<span class="chip-confirm-text"></span><button type="button" class="btn danger" data-act="remove">Remove</button><button type="button" class="btn" data-act="cancel">Cancel</button>`;
  railEl.append(confirmEl);
  let confirmFor: number | null = null;
  const closeConfirm = () => {
    confirmFor = null;
    confirmEl.hidden = true;
  };
  const confirmRemove = (vrx: number, label: string, chip: HTMLElement) => {
    confirmFor = vrx;
    confirmEl.querySelector(".chip-confirm-text")!.textContent = `Remove ${label}?`;
    confirmEl.hidden = false;
    // Under the chip, kept inside the rail's width.
    const railBox = railEl.getBoundingClientRect();
    const chipBox = chip.getBoundingClientRect();
    const left = Math.max(0, Math.min(chipBox.left - railBox.left, railBox.width - confirmEl.offsetWidth));
    confirmEl.style.left = `${left}px`;
    confirmEl.style.top = `${chipBox.bottom - railBox.top}px`;
  };
  confirmEl.addEventListener("click", (ev) => {
    const act = (ev.target as Element).closest<HTMLElement>("[data-act]")?.dataset.act;
    if (act === "remove" && confirmFor !== null) receivers.remove(confirmFor);
    if (act) closeConfirm();
  });
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
      const mode = modeLabel(r.mode);
      b.textContent = `${formatMHz(r.hz)} ${mode}${flag}`;
      const tags: { text: string; warn: boolean }[] = [];
      if (r.vrx === active) {
        const on = labelAt(r.hz, chipDetections);
        if (on) tags.push({ text: labelText(on), warn: false });
      }
      const call = voiceStatus(r.mode, catalogue?.voiceModes ?? null, Date.now() - (encryptedAt.get(r.vrx) ?? -Infinity) < ENCRYPTED_HOLD_MS);
      if (call) tags.push({ text: call, warn: true });
      for (const t of tags) {
        const s = document.createElement("span");
        s.className = t.warn ? "rx-tag warn" : "rx-tag";
        s.textContent = t.text;
        b.append(s);
      }
      const note = r.audioError ?? (r.statusUnavailable ? "the gateway could not read this receiver back; shown as sent" : "");
      const said = [note, ...tags.map((t) => t.text)].filter((s) => s !== "").join(", ");
      b.setAttribute("aria-label", `Receiver ${formatMHz(r.hz)} MHz ${mode}${said ? `, ${said}` : ""}`);
      if (note) b.title = note;
      const press = new ChipPress({
        longPress: () => {
          dial.stop();
          confirmRemove(r.vrx, `${formatMHz(r.hz)} ${mode}`, b);
        },
        schedule: (fn, ms) => {
          const h = window.setTimeout(fn, ms);
          return () => window.clearTimeout(h);
        },
      });
      b.addEventListener("pointerdown", (ev) => press.down(ev.clientX, ev.clientY));
      b.addEventListener("pointermove", (ev) => press.move(ev.clientX, ev.clientY));
      b.addEventListener("pointerup", () => press.up());
      b.addEventListener("pointercancel", () => press.up());
      b.addEventListener("contextmenu", (ev) => ev.preventDefault());
      // A tap in its own handler, so live audio may start from it.
      b.addEventListener("click", () => {
        if (!press.click()) return;
        dial.stop();
        receivers.select(r.vrx);
      });
      chipsEl.append(b);
    }
    railEl.hidden = list.length === 0;
    if (confirmFor !== null && !list.some((r) => r.vrx === confirmFor)) closeConfirm();
    const current = active === null ? null : receivers.list.get(active) ?? null;
    top.show(current);
    sheet!.show(current, list.length > 0);
    // Replacement, removal, resume and a restart's remap all end up here.
    dial.check();
    dial.draw();
  };
  receivers.list.onChange(renderReceivers);
  renderReceivers();
  // A drag, dial or digit swipe previews through the list, so the passband
  // follows the finger on the next frame without waiting for the gateway.
  const updateMarks = () => {
    receiverMarks = receivers.list.list();
    activeMark = receivers.list.active;
    trace.setReceivers(receiverMarks, activeMark);
    requestDraw();
  };
  receivers.list.onChange(updateMarks);
  updateMarks();

  // ---- Status --------------------------------------------------------------
  const status = new StatusModel();
  const statusChips = root.querySelector<HTMLElement>("#status-chips")!;
  const renderStatus = () => {
    stage.classList.toggle("frozen", status.frozen());
    statusChips.textContent = "";
    for (const c of status.problems()) {
      const d = document.createElement("div");
      d.className = c.stops ? "status-chip stops" : "status-chip";
      d.textContent = c.text;
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
  // The desktop's colours. A malformed event is ignored and the last good
  // colours stay, the build's defaults before any arrives.
  link.on("display", (e) => {
    const colours = parseDisplay(e);
    if (!colours) return;
    waterfall.setPalette(colours.waterfall);
    trace.setColours(colours);
    requestDraw();
  });
  // The chips are rebuilt only when the active receiver's label text moves,
  // not on every poll.
  let chipLabel = "";
  link.on("detections", (e) => {
    detections = parseDetections(e);
    chipDetections = detections;
    const act = receivers.list.active === null ? undefined : receivers.list.get(receivers.list.active);
    const on = act ? labelAt(act.hz, detections) : null;
    const nextLabel = on ? labelText(on) : "";
    if (nextLabel !== chipLabel) {
      chipLabel = nextLabel;
      renderReceivers();
    }
    signals.update(detections);
    const boxes = detectionBoxes(detections);
    waterfall.setDetections(boxes, boxCarryRows(pacer.interval));
    trace.setDetections(boxes);
    requestDraw();
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
  let centreDragHz: number | null = null;
  // The view follows only once the receiver is this close to an edge, so a
  // small nudge does not slide the picture under the finger.
  const EDGE_FRACTION = 0.1;

  attachGestures(
    body,
    {
      dragTune: (dHz) => {
        if (!Number.isFinite(dHz) || swiping) return;
        // No receiver: the drag moves the radio, the way dragging the band
        // would, so a drag to the right goes down in frequency.
        if (receivers.list.active === null && dragHz === null) {
          if (!band) return;
          if (centreDragHz === null) dial.stop();
          centreDragHz = clampToRanges((centreDragHz ?? centrePreviewHz ?? band.centerHz) - dHz, radio.tuneRanges());
          showCentre(Math.round(centreDragHz));
          centreLimiter.set(centreDragHz);
          return;
        }
        let next = dragHz;
        if (next === null) {
          dial.stop();
          const id = receivers.list.active;
          const r = id === null ? undefined : receivers.list.get(id);
          if (!r) return;
          next = r.hz;
        }
        next = Math.max(1, next + dHz);
        // The marker and chip move now; the engine confirms through status.
        // Only a drag that took effect accumulates.
        const shown = receivers.preview(next);
        if (!shown) return;
        dragHz = shown.hz;
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
        if (centreDragHz !== null) {
          centreDragHz = null;
          centreLimiter.flush();
          return;
        }
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
        if (!Number.isFinite(hz)) return;
        dial.stop();
        receivers.tap(snapTap(hz, detections));
      },
      longPress: (hz) => {
        if (!Number.isFinite(hz)) return;
        dial.stop();
        receivers.longPress(hz);
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
    receivers.setSource(e);
    centre.settled();
    renderStatus();
    if (e.open !== true || typeof e.centerHz !== "number" || typeof e.rateHz !== "number") {
      band = null;
      limits = null;
      return;
    }
    const changed = !band || band.centerHz !== e.centerHz || band.rateHz !== e.rateHz;
    band = { centerHz: e.centerHz, rateHz: e.rateHz };
    const nextLimits = viewBand(e.centerHz, e.rateHz, e.dataLowHz, e.dataHighHz);
    const limitsMoved = !limits || limits.centerHz !== nextLimits.centerHz || limits.rateHz !== nextLimits.rateHz;
    limits = nextLimits;
    if (!changed && view) {
      // Same band, edges newly known (the spectrum switched on): re-clamp.
      if (limitsMoved) apply(view);
      return;
    }
    // A retuned or reopened source: history was cut for another band.
    waterfall.clear();
    forgetRows();
    apply({ centerHz: limits.centerHz, spanHz: limits.rateHz, widthPx: widthDev });
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
    newestIndex = Number(f.sourceStart);
    // Frozen means the engine is gone: whatever trickles in is not news.
    if (status.frozen()) return;
    // A context loss or the 2D handover drops history, and the rows it kept
    // are not the ones the clock holds.
    if (waterfall.state !== rendererState) {
      rendererState = waterfall.state;
      forgetRows();
    }
    if (waterfall.state !== "lost" && clock.push(Number(f.sourceStart))) captionStore.clear();
    // Both rows arrive already in the desktop's windows; nothing is rescaled
    // here.
    const now = performance.now();
    waterfall.pushRow(f);
    if (waterfall.state !== "lost") pacer.onRow(now);
    trace.setRow(f, now);
    const active = receivers.list.active;
    const r = active === null ? undefined : receivers.list.get(active);
    if (r) {
      const pb = passbandHz(r.mode, r.hz, r.bandwidthHz ?? null);
      top.level(levelInBand(f, pb.lowHz, pb.highHz));
    }
    requestDraw();
  });

  wireLifecycle(link, document, window);
  link.start();
  return { link, audio, keep, receivers, view: () => view };
}
