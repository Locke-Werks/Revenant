// The waterfall contrast curve, input across and output up. Each piece of
// the line, and the strip along the bottom, is drawn in the colour the
// waterfall gives that output. The dim diagonal is the straight map and the
// dotted line is the pivot. Redraws whenever contrast or pivot moves. Used in
// the settings panel and as the pivot strip's popup on the waterfall.

import QtQuick
import Revenant

Canvas {
    id: curveCanvas

    Connections {
        target: ScaleSettings
        function onWaterfallContrastChanged() { curveCanvas.requestPaint() }
        function onWaterfallPivotChanged() { curveCanvas.requestPaint() }
    }

    onPaint: {
        const ctx = getContext("2d")
        const w = width, h = height, pad = 4
        const iw = w - 2 * pad, ih = h - 2 * pad
        ctx.reset()
        ctx.fillStyle = Theme.panelSolid
        ctx.fillRect(0, 0, w, h)
        ctx.strokeStyle = Theme.border
        ctx.lineWidth = 1
        ctx.strokeRect(0.5, 0.5, w - 1, h - 1)

        // The output colour along the bottom strip, so the input
        // band below the pivot reads as how dark it ends up.
        const steps = 120
        const strip = 6
        for (let i = 0; i < steps; ++i) {
            const x = (i + 0.5) / steps
            ctx.fillStyle = ScaleSettings.paletteAt(ScaleSettings.curveAt(x))
            ctx.fillRect(pad + i * iw / steps, h - pad - strip, iw / steps + 1, strip)
        }

        ctx.strokeStyle = Theme.inkOff
        ctx.beginPath()
        ctx.moveTo(pad, h - pad)
        ctx.lineTo(w - pad, pad)
        ctx.stroke()

        const p = ScaleSettings.waterfallPivot
        ctx.strokeStyle = Theme.inkDim
        ctx.setLineDash([2, 3])
        ctx.beginPath()
        ctx.moveTo(pad + p * iw, pad)
        ctx.lineTo(pad + p * iw, h - pad - strip)
        ctx.stroke()
        ctx.setLineDash([])

        ctx.lineWidth = 2.5
        let lx = pad, ly = h - pad - ScaleSettings.curveAt(0) * ih
        for (let i = 1; i <= steps; ++i) {
            const x = i / steps
            const y = ScaleSettings.curveAt(x)
            const px = pad + x * iw, py = h - pad - y * ih
            ctx.strokeStyle = ScaleSettings.paletteAt(Math.max(y, 0.08))
            ctx.beginPath()
            ctx.moveTo(lx, ly)
            ctx.lineTo(px, py)
            ctx.stroke()
            lx = px
            ly = py
        }
    }

    Component.onCompleted: requestPaint()
}
