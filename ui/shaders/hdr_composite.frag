#version 440

// The main window's last pass while it has an scRGB swapchain;
// render/hdr_output.h has why it exists.
//
// Everything above it drew exactly as it does in SDR, sRGB-encoded and blended
// in that encoding, into a half-float layer. Only the span's colour map goes
// past 1.0 there. This decodes the lot with the sRGB curve, extended past 1.0,
// and scales it so 1.0 lands on the SDR white level Windows is set to, which is
// where every other SDR window on the desktop puts its white.
//
// The window's clear colour is not in the layer, and an scRGB swapchain would
// read Theme.background as linear and show it as a lifted grey, so whatever the
// layer leaves uncovered is filled with it here, in the same encoding.

layout(location = 0) in vec2 qt_TexCoord0;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float sdrScale;
    vec4 backdrop;
};

layout(binding = 1) uniform sampler2D source;

vec3 decode(vec3 c)
{
    vec3 a = abs(c);
    vec3 lo = a / 12.92;
    vec3 hi = pow((a + 0.055) / 1.055, vec3(2.4));
    return sign(c) * mix(lo, hi, step(0.04045, a));
}

void main()
{
    vec4 layer = texture(source, qt_TexCoord0);
    vec3 encoded = layer.rgb + (1.0 - layer.a) * backdrop.rgb;
    fragColor = vec4(decode(encoded) * sdrScale, 1.0);
}
