import { test } from "node:test";
import assert from "node:assert/strict";
import { placeLabels } from "../src/label_placement";

test("a label centres over its band even when the band is narrower than the text", () => {
  assert.deepEqual(placeLabels([{ x0: 100, x1: 102, width: 30 }], 400), [101]);
});

test("a label that would overlap the one placed to its left is skipped", () => {
  const at = placeLabels(
    [
      { x0: 100, x1: 102, width: 30 },
      { x0: 110, x1: 112, width: 30 },
      { x0: 200, x1: 202, width: 30 },
    ],
    400,
    4,
  );
  assert.deepEqual(at, [101, null, 201]);
});

test("placement runs left to right whatever order the bands arrive in", () => {
  const at = placeLabels(
    [
      { x0: 110, x1: 112, width: 30 },
      { x0: 100, x1: 102, width: 30 },
    ],
    400,
  );
  assert.deepEqual(at, [null, 101]);
});

test("a label at the canvas edge is pulled inside it", () => {
  assert.deepEqual(placeLabels([{ x0: 0, x1: 2, width: 30 }], 400), [15]);
  assert.deepEqual(placeLabels([{ x0: 398, x1: 400, width: 30 }], 400), [385]);
});

test("bands with no label, or a label wider than the canvas, take no room", () => {
  const at = placeLabels(
    [
      { x0: 100, x1: 102, width: 0 },
      { x0: 104, x1: 106, width: 20 },
      { x0: 0, x1: 400, width: 500 },
    ],
    400,
  );
  assert.deepEqual(at, [null, 105, null]);
});

test("the gap is kept between neighbours", () => {
  // Right edge of the first at 115; the second's left edge at 117 is inside a 4 px gap.
  assert.deepEqual(placeLabels([{ x0: 100, x1: 100, width: 30 }, { x0: 132, x1: 132, width: 30 }], 400, 4), [100, null]);
  assert.deepEqual(placeLabels([{ x0: 100, x1: 100, width: 30 }, { x0: 134, x1: 134, width: 30 }], 400, 4), [100, 134]);
});
