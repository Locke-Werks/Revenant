// Everything the main window draws, while it has an HDR swapchain: the window's
// content and its popup overlay are moved under this by render/hdr_output.cpp,
// and the layer here hands the lot to HdrComposite.qml, which has the argument.
//
// An item of its own rather than a layer on the window's root item, because
// the root item is made by the window and has no QML context, and a layer makes
// its effect in its item's context.

import QtQuick

Item {
    layer.enabled: true
    // Half float, because the colour map goes past 1.0 there and an 8-bit
    // layer would clip it.
    layer.format: ShaderEffectSource.RGBA16F
    layer.effect: HdrComposite {}
}
