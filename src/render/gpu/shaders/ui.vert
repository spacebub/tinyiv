#version 450

layout(push_constant) uniform Push {
    vec2 screen;
    float scale;
} push;

layout(location = 0) in vec2 position;
layout(location = 1) in vec2 coordinate;
layout(location = 2) in vec4 tint;

layout(location = 0) out vec2 texel;
layout(location = 1) out vec4 shade;

void main() {
    texel = coordinate;
    shade = tint;
    gl_Position = vec4(((position / push.screen) * 2.0) - 1.0, 0.0, 1.0);
}
