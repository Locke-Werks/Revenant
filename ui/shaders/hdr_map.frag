#version 440

// The span displays' colour map, given headroom above SDR white when the
// window has an HDR swapchain. render/hdr_map.h has the argument.
//
// The level is recovered from the colour itself: the map's OKLab lightness
// rises in a straight line with the level (render/colour_map.h), so the
// lightness of a texel says where on the map it is without the texture having
// to carry the level beside it. Only the top of the map is lifted, and only in
// brightness: the colour is scaled in linear light, so hue and chroma are the
// map's own.
//
// Output stays sRGB-encoded, extended past 1.0 with the same curve, because the
// window's composite pass (hdr_composite.frag) decodes everything it is handed
// with that one curve. With no headroom the texel passes through untouched.

layout(location = 0) in vec2 texCoord;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float headroom;
    float knee;
    float bottomL;
    float topL;
};

layout(binding = 1) uniform sampler2D map;

vec3 decode(vec3 c)
{
    vec3 a = abs(c);
    vec3 lo = a / 12.92;
    vec3 hi = pow((a + 0.055) / 1.055, vec3(2.4));
    return sign(c) * mix(lo, hi, step(0.04045, a));
}

vec3 encode(vec3 l)
{
    vec3 a = abs(l);
    vec3 lo = a * 12.92;
    vec3 hi = 1.055 * pow(a, vec3(1.0 / 2.4)) - 0.055;
    return sign(l) * mix(lo, hi, step(0.0031308, a));
}

// Ottosson's matrices, as render/colour_map.h has them.
float oklab_lightness(vec3 lin)
{
    float l = 0.4122214708 * lin.r + 0.5363325363 * lin.g + 0.0514459929 * lin.b;
    float m = 0.2119034982 * lin.r + 0.6806995451 * lin.g + 0.1073969566 * lin.b;
    float s = 0.0883024619 * lin.r + 0.2817188376 * lin.g + 0.6299787005 * lin.b;
    vec3 lms = pow(max(vec3(l, m, s), vec3(0.0)), vec3(1.0 / 3.0));
    return 0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720412 * lms.z;
}

void main()
{
    vec4 texel = texture(map, texCoord);
    if (headroom > 1.0 && texel.a > 0.0) {
        vec3 lin = decode(texel.rgb / texel.a);
        float level = clamp((oklab_lightness(lin) - bottomL) / (topL - bottomL), 0.0, 1.0);
        float lift = smoothstep(knee, 1.0, level);
        lin *= 1.0 + (headroom - 1.0) * lift * lift;
        texel.rgb = encode(lin) * texel.a;
    }
    fragColor = texel * qt_Opacity;
}
