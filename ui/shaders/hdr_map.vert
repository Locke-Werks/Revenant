#version 440

// render/hdr_map.h. The block is the fragment stage's, member for member.

layout(location = 0) in vec4 qt_VertexPosition;
layout(location = 1) in vec2 qt_VertexTexCoord;

layout(location = 0) out vec2 texCoord;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float headroom;
    float knee;
    float bottomL;
    float topL;
};

out gl_PerVertex { vec4 gl_Position; };

void main()
{
    texCoord = qt_VertexTexCoord;
    gl_Position = qt_Matrix * qt_VertexPosition;
}
