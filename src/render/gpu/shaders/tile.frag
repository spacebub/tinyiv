#version 450

layout(set = 0, binding = 0) uniform sampler2DArray tiles;

layout(location = 0) in vec2 texel;
layout(location = 1) flat in vec4 limit;
layout(location = 2) flat in float slice;

layout(location = 0) out vec4 colour;

// Sharp bilinear, as SDL_SCALEMODE_PIXELART. Tiles share layers, so sampling is clamped to the tile.
void main() {
    const vec2 size = vec2(textureSize(tiles, 0).xy);
    const vec2 box = clamp(fwidth(texel), 1e-5, 1.0);
    const vec2 start = texel - (0.5 * box);
    const vec2 offset = smoothstep(1.0 - box, vec2(1.0), fract(start));
    const vec2 at = clamp(floor(start) + 0.5 + offset, limit.xy + 0.5, limit.zw - 0.5);
    const vec4 sampled = textureGrad(tiles, vec3(at / size, slice), dFdx(texel / size), dFdy(texel / size));

    colour = sampled;
}
