import { test } from "node:test";
import assert from "node:assert/strict";
import { StatusModel } from "../src/ui/status";

test("a cold start shows the engine starting until the link is up", () => {
  const s = new StatusModel();
  s.starting(true);
  assert.equal(s.pill(), "Starting the engine...");
  s.link("waiting", "the engine starts when a phone connects");
  assert.equal(s.pill(), "Starting the engine...");
  s.ready(false);
  s.link("up");
  assert.equal(s.pill(), null);
});

test("a dropped socket ends the starting notice", () => {
  const s = new StatusModel();
  s.starting(true);
  s.socket(false);
  assert.equal(s.pill(), "connecting");
});
