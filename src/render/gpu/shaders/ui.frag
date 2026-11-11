#version 450

// Fills sample a white block in the atlas.
layout(set = 0, binding = 0) uniform sampler2D atlas;

layout(location = 0) in vec2 texel;
layout(location = 1) in vec4 shade;

layout(location = 0) out vec4 colour;

void main() {
    const vec2 size = vec2(textureSize(atlas, 0));
    const vec2 box = clamp(fwidth(texel), 1e-5, 1.0);
    const vec2 start = texel - (0.5 * box);
    const vec2 offset = smoothstep(1.0 - box, vec2(1.0), fract(start));
    const vec2 at = floor(start) + 0.5 + offset;
    const vec4 lit = textureGrad(atlas, at / size, dFdx(texel / size), dFdy(texel / size));

    // Unlit texels are transparent black, so a glyph edge darkens as it fades and reads sharp.
    colour = vec4(shade.rgb * lit.rgb, shade.a * lit.a);
}
