// Samples the host's premultiplied BGRA overlay texture. The blend state does
// the (ONE, ONE_MINUS_SRC_ALPHA) composition, so there is no alpha maths here.
#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D overlayTex;

void main() {
    outColor = texture(overlayTex, vUV);
}
