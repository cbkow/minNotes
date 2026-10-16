#version 440
layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec2 v_a;
layout(location = 2) in vec2 v_b;
layout(location = 3) in float v_halfW;
layout(location = 4) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    float pxScale;
} ubuf;

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-12), 0.0, 1.0);
    return length(pa - ba * h);
}

void main() {
    float d = sdSegment(v_pos, v_a, v_b) - v_halfW;        // local px outside the capsule
    float cov = clamp(0.5 - d * ubuf.pxScale, 0.0, 1.0);    // one screen px of feather
    vec4 c = v_color;
    c.rgb *= c.a;                                           // premultiplied for the scene graph's blend
    fragColor = c * cov * ubuf.qt_Opacity;
}
