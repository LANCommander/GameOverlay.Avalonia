#version 450
// Samples the (premultiplied BGRA, uploaded as B8G8R8A8_UNORM) overlay image.
// The render pass blends this over the game's frame with (ONE, 1-SRC_ALPHA).
layout(location = 0) in vec2 vTex;
layout(location = 0) out vec4 frag;
layout(binding = 0) uniform sampler2D uTex;

void main() {
    frag = texture(uTex, vTex);
}
