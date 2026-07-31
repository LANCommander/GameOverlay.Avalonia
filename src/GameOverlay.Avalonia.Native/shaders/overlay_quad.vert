#version 450
// Fullscreen quad generated from gl_VertexIndex (0..3, triangle strip) - no
// vertex buffer needed. Vulkan NDC Y points down and the framebuffer origin is
// top-left, so texcoord = position maps the image's top-left texel to the top
// of the screen with no flip.
layout(location = 0) out vec2 vTex;

void main() {
    vec2 p = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
    vTex = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
