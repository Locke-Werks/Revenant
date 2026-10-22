// One of the icons in icons.js, drawn at the item's size in one colour.
//
// The paths are on a 24 unit grid and the shape is scaled to the item, so an
// icon is as sharp at 150% as at 100%: the curve renderer antialiases the
// outline itself, at the screen's pixel ratio, with no multisampled layer.

import QtQuick
import QtQuick.Shapes
import "icons.js" as Icons

Item {
    id: icon

    property string name
    property color color: "white"

    implicitWidth: 16
    implicitHeight: 16

    Shape {
        width: 24
        height: 24
        scale: Math.min(icon.width, icon.height) / 24
        transformOrigin: Item.TopLeft
        x: (icon.width - 24 * scale) / 2
        y: (icon.height - 24 * scale) / 2
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: icon.color
            strokeWidth: 1.75
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: Icons.stroke(icon.name) }
        }

        ShapePath {
            strokeColor: "transparent"
            strokeWidth: 0
            fillColor: icon.color
            PathSvg { path: Icons.fill(icon.name) }
        }
    }
}
