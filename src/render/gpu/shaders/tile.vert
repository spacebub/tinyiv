#version 450

layout(push_constant) uniform Push {
    vec2 screen;
    float scale;
} push;

layout(location = 0) in vec4 target;
// Corners in strip order: top left, top right, bottom left, bottom right.
layout(location = 1) in vec4 corners01;
layout(location = 2) in vec4 corners23;
// The texels of the layer that the tile fills: left, top, right, bottom.
layout(location = 3) in vec4 bounds;
layout(location = 4) in float layer;

layout(location = 0) out vec2 texel;
layout(location = 1) flat out vec4 limit;
layout(location = 2) flat out float slice;

void main() {
    const int corner = gl_VertexIndex;
    const vec2 unit = vec2(float(corner & 1), float(corner >> 1));
    const vec2 position = target.xy + (unit * target.zw);
    const vec4 pair = corner < 2 ? corners01 : corners23;

    texel = (corner & 1) == 0 ? pair.xy : pair.zw;
    limit = bounds;
    slice = layer;
    gl_Position = vec4(((position / push.screen) * 2.0) - 1.0, 0.0, 1.0);
}
