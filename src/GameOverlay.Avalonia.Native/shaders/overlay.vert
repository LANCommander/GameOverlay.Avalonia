// Fullscreen triangle generated from gl_VertexIndex - no vertex buffer, no
// input layout, matching the D3D11 and D3D12 compositors.
//
// Vulkan's clip space has +Y pointing down, so uv (0,0) maps to NDC (-1,-1)
// which is the TOP-left. That is what puts row 0 of the overlay texture at the
// top of the screen without an explicit flip.
#version 450

layout(location = 0) out vec2 vUV;

void main() {
    vUV = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
}
