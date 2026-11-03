import { test } from "node:test";
import assert from "node:assert/strict";
import { formatSince, leaseScreenText } from "../src/ui/lease_screen";

test("nothing to show while the phone holds the engine", () => {
  assert.equal(leaseScreenText(null), null);
  assert.equal(leaseScreenText({ event: "lease", state: "held", holder: "phone 10.1.10.57 via webgate" }), null);
});

test("in use names the holder and the time, and offers Take over", () => {
  const now = new Date(2026, 10, 3, 12, 0, 0);
  const since = new Date(2026, 10, 3, 9, 14, 0);
  const text = leaseScreenText(
    { event: "lease", state: "inUse", holder: "Revenant desktop on VALKYRIE", since: since.toISOString() },
    now,
  );
  if (text === null) throw new Error("no notice");
  assert.equal(text.button, "Take over");
  assert.ok(text.detail.startsWith("Revenant is in use by Revenant desktop on VALKYRIE since "));
  assert.ok(text.detail.endsWith(formatSince(since.toISOString(), now)));
});

test("taken over names who took it and offers Take back", () => {
  const text = leaseScreenText({ event: "lease", state: "lost", holder: "phone 10.1.10.58 via webgate" });
  assert.deepEqual(text, { title: "Taken over", detail: "Taken over by phone 10.1.10.58 via webgate", button: "Take back" });
});

test("a time that does not parse is shown as the gateway sent it", () => {
  assert.equal(formatSince("sometime"), "sometime");
  const now = new Date(2026, 10, 3, 12, 0, 0);
  const yesterday = new Date(2026, 10, 2, 22, 5, 0);
  // Another day carries its date.
  assert.equal(formatSince(yesterday.toISOString(), now) !== formatSince(yesterday.toISOString(), yesterday), true);
});
