// The main window's layer effect while it has an HDR swapchain. Made by
// render/hdr_output.cpp and set on the root item's layer from there, because
// the root item is above anything a QML file can reach; the shader has the
// argument.

import QtQuick

ShaderEffect {
    // The layer's texture, by the default layer.samplerName.
    property variant source

    property real sdrScale: hdrOutput.compositeScale
    property color backdrop: Theme.background

    fragmentShader: "qrc:/shaders/hdr_composite.frag.qsb"
}
