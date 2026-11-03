// A stand-in for the iPhone app, for scripts/frame-budget.ps1 -Phone.
//
// It speaks the wire format in web/src/protocol.ts to a running
// revenant-webgate and does what the app does when it is open on a receiver:
// sets a phone-sized view, adds an NFM receiver, turns its audio on, answers
// the gateway's ping with a pong, and then just receives. It decodes nothing
// and draws nothing, so what it costs the gateway and the engine is the
// network and encode side only, which is the part a frame budget cares about.
//
// Needs Node 22 or later, whose global WebSocket this uses. The gateway's
// certificate is the locally issued CA's and Node does not trust it, so
// NODE_TLS_REJECT_UNAUTHORIZED=0 is set by the caller. A script sends no
// Origin header and the gateway lets that through.
//
//   node scripts/webgate-phone-sim.mjs wss://127.0.0.1:27700/ws 70 out.json

const [url, secondsArg, outPath] = process.argv.slice(2);
if (!url || !secondsArg) {
  console.error("usage: webgate-phone-sim.mjs URL SECONDS [OUT.json]");
  process.exit(2);
}
const seconds = Number(secondsArg);

const stats = {
  url,
  seconds,
  spectrumFrames: 0,
  spectrumBytes: 0,
  opusPackets: 0,
  opusBytes: 0,
  pings: 0,
  events: {},
  vrx: null,
  errors: [],
};

let nextId = 1;
const pending = new Map();
const ws = new WebSocket(url);
ws.binaryType = "arraybuffer";

function request(op, args) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    const timer = setTimeout(() => {
      pending.delete(id);
      reject(new Error(`${op} timed out`));
    }, 15000);
    pending.set(id, { resolve, reject, timer });
    ws.send(JSON.stringify(args ? { id, op, args } : { id, op }));
  });
}

ws.addEventListener("message", (e) => {
  if (typeof e.data !== "string") {
    const tag = new DataView(e.data).getUint8(0);
    if (tag === 1) {
      stats.spectrumFrames += 1;
      stats.spectrumBytes += e.data.byteLength;
    } else if (tag === 2) {
      stats.opusPackets += 1;
      stats.opusBytes += e.data.byteLength;
    }
    return;
  }
  const msg = JSON.parse(e.data);
  if (typeof msg.event === "string") {
    stats.events[msg.event] = (stats.events[msg.event] ?? 0) + 1;
    if (msg.event === "ping") {
      stats.pings += 1;
      ws.send(JSON.stringify({ id: nextId++, op: "pong" }));
    }
    return;
  }
  const waiter = pending.get(msg.id);
  if (!waiter) return;
  pending.delete(msg.id);
  clearTimeout(waiter.timer);
  if (msg.ok) waiter.resolve(msg.result);
  else waiter.reject(new Error(msg.error));
});

function finish(code) {
  const text = JSON.stringify(stats, null, 2);
  if (outPath) {
    import("node:fs").then((fs) => fs.writeFileSync(outPath, text)).finally(() => process.exit(code));
  } else {
    console.log(text);
    process.exit(code);
  }
}

ws.addEventListener("error", () => {
  stats.errors.push("socket error");
});
ws.addEventListener("close", () => {
  stats.errors.push("socket closed");
  finish(1);
});

ws.addEventListener("open", async () => {
  try {
    // iPhone 15 portrait, 393 pt at 3x.
    await request("setView", { centerHz: 100e6, spanHz: 20e6, widthPx: 1179 });
    const added = await request("addVrx", { hz: 100.3e6, mode: "nfm" });
    stats.vrx = added.vrx;
    await request("audio", { vrx: added.vrx, on: true });
  } catch (err) {
    stats.errors.push(String(err));
    finish(1);
    return;
  }
  setTimeout(() => {
    finish(stats.errors.length === 0 ? 0 : 1);
  }, seconds * 1000);
});
