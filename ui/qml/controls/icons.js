// The button icons, as SVG path data on a 24 by 24 grid.
//
// Paths rather than SVG files so they draw through QtQuick.Shapes: vector on
// the GPU at whatever the screen's scale is, tinted by a binding rather than
// an effect layer, and with no image plugin to deploy. The shapes are the
// ones the owner approved on the prototype sheet on 2026-10-22.
//
// Each entry is {stroke, fill}: stroke is drawn with the icon's ink at a
// stroke width of 1.75 grid units, fill is filled with it.

.pragma library

function circle(cx, cy, r) {
    return "M" + (cx - r) + " " + cy + "a" + r + " " + r + " 0 1 0 " + (2 * r) + " 0"
         + "a" + r + " " + r + " 0 1 0 " + (-2 * r) + " 0z"
}

function rect(x, y, w, h, r) {
    return "M" + (x + r) + " " + y + "h" + (w - 2 * r) + "q" + r + " 0 " + r + " " + r
         + "v" + (h - 2 * r) + "q0 " + r + " " + (-r) + " " + r
         + "h" + (-(w - 2 * r)) + "q" + (-r) + " 0 " + (-r) + " " + (-r)
         + "v" + (-(h - 2 * r)) + "q0 " + (-r) + " " + r + " " + (-r) + "z"
}

const speakerBody = "M11 5L6 9H3v6h3l5 4z"
const bookmark = "M6 3h12v18l-6-4-6 4z"
const frame = "M18 14v5a1 1 0 0 1-1 1H5a1 1 0 0 1-1-1V7a1 1 0 0 1 1-1h5"

const shapes = {
    "gear": { stroke: circle(12, 12, 3)
        + "M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z" },
    "bookmark": { stroke: bookmark },
    "bookmark-plus": { stroke: bookmark + "M12 7v6M9 10h6" },
    "record": { stroke: circle(12, 12, 8.5), fill: circle(12, 12, 3.5) },
    "antenna": { stroke: "M12 11v10M8 21h8" + circle(12, 9, 2)
        + "M7.8 4.8a6 6 0 0 0 0 8.4M16.2 4.8a6 6 0 0 1 0 8.4M5 2a10 10 0 0 0 0 14M19 2a10 10 0 0 1 0 14" },
    "receivers": { stroke: rect(3, 4, 18, 4, 1) + rect(3, 10, 18, 4, 1) + rect(3, 16, 18, 4, 1)
        + "M6 4v4M6 10v4M6 16v4" },
    "captions": { stroke: rect(3, 5, 18, 14, 2) + "M7 13h4M13 13h4M7 16h7" },
    "chevron-down": { stroke: "M6 9l6 6 6-6" },
    "chevron-right": { stroke: "M9 6l6 6-6 6" },
    "expand": { stroke: "M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5" },
    "tag": { stroke: "M3 12V4a1 1 0 0 1 1-1h8l9 9-9 9z" + circle(8, 8, 1.5) },
    "waterfall": { stroke: rect(3, 3, 18, 18, 2) + "M3 8h18M3 13h18M3 18h18M12 3v18" },
    "persist": { stroke: "M3 18c3 0 3-10 6-10s3 7 6 7 3-9 6-9M3 21c3 0 3-6 6-6s3 4 6 4 3-6 6-6" },
    "cube": { stroke: "M12 2l9 5v10l-9 5-9-5V7zM12 22V12M21 7l-9 5-9-5" },
    "x": { stroke: "M6 6l12 12M18 6L6 18" },
    "live": { stroke: "M6.3 6.3a8 8 0 0 0 0 11.4M17.7 6.3a8 8 0 0 1 0 11.4", fill: circle(12, 12, 4) },
    "unlock": { stroke: rect(4, 11, 16, 10, 2) + "M8 11V7a4 4 0 0 1 7.5-2" },
    "plus": { stroke: "M12 5v14M5 12h14" },
    "minus": { stroke: "M5 12h14" },
    "trash": { stroke: "M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3" },
    "eraser": { stroke: "M20 20H9l-6-6 10-10 8 8-6 6M8 9l7 7" },
    "popout": { stroke: "M14 4h6v6M20 4l-9 9" + frame },
    "dock": { stroke: "M10 14L20 4M10 8v6h6" + frame },
    "mute": { stroke: speakerBody + "M16 9l5 6M21 9l-5 6" },
    "speaker": { stroke: speakerBody + "M15.5 8.5a5 5 0 0 1 0 7M18.5 5.5a9 9 0 0 1 0 13" },
    "headphones": { stroke: "M4 16v-4a8 8 0 0 1 16 0v4" + rect(3, 15, 4, 6, 1) + rect(17, 15, 4, 6, 1) },
    "reset": { stroke: "M3 12a9 9 0 1 0 3-6.7M3 4v5h5" },
    "copy": { stroke: rect(8, 8, 12, 12, 2) + "M16 8V5a1 1 0 0 0-1-1H5a1 1 0 0 0-1 1v10a1 1 0 0 0 1 1h3" },
    "export": { stroke: "M12 3v12M7 10l5 5 5-5M4 17v3h16v-3" },
    "folder": { stroke: "M3 6a1 1 0 0 1 1-1h5l2 2h9a1 1 0 0 1 1 1v10a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1z" },
    "undo": { stroke: "M9 14L4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3" },
    "eye": { stroke: "M2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z" + circle(12, 12, 3) },
    "pencil": { stroke: "M4 20h4L19 9l-4-4L4 16zM13 7l4 4" },
    "chip": { stroke: rect(6, 6, 12, 12, 1.5)
        + "M9 2v4M15 2v4M9 18v4M15 18v4M2 9h4M2 15h4M18 9h4M18 15h4" },
    "crosshair": { stroke: circle(12, 12, 8) + "M12 2v5M12 17v5M2 12h5M17 12h5" },
    "list": { stroke: "M9 6h12M9 12h12M9 18h12",
        fill: circle(4.5, 6, 1.2) + circle(4.5, 12, 1.2) + circle(4.5, 18, 1.2) },
    "wave": { stroke: "M2 12h3l2-6 3 12 3-9 2 5 2-2h5" },
    "text": { stroke: "M4 6h16M4 11h16M4 16h10" },
    "gain": { stroke: "M4 20h16M7 16V12M12 16V8M17 16V4" },
    "check": { stroke: "M5 12l5 5 9-10" },
    "notch": { stroke: "M2 8h7l1.5 9h3L15 8h7" },
    "filter": { stroke: "M3 5h18l-7 8v6l-4 2v-8z" },
    "power": { stroke: "M12 3v9M6.4 6.4a8 8 0 1 0 11.2 0" }
}

function stroke(name) {
    const s = shapes[name]
    return s ? s.stroke : ""
}

function fill(name) {
    const s = shapes[name]
    return s && s.fill ? s.fill : ""
}
