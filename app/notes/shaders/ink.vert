#version 440
// Canvas ink (B5): capsule quads. Each vertex carries its segment and half width through to the
// fragment stage, which evaluates the signed distance and feathers one screen px (ubuf.pxScale =
// screen px per local px).
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_a;
layout(location = 2) in vec2 a_b;
layout(location = 3) in float a_halfW;
layout(location = 4) in vec4 a_color;

layout(location = 0) out vec2 v_pos;
layout(location = 1) out vec2 v_a;
layout(location = 2) out vec2 v_b;
layout(location = 3) out float v_halfW;
layout(location = 4) out vec4 v_color;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float pxScale;
} ubuf;

out gl_PerVertex { vec4 gl_Position; };

void main() {
    v_pos = a_pos;
    v_a = a_a;
    v_b = a_b;
    v_halfW = a_halfW;
    v_color = a_color;
    gl_Position = ubuf.qt_Matrix * vec4(a_pos, 0.0, 1.0);
}
