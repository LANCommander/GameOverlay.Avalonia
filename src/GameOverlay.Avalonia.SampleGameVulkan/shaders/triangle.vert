#version 450

layout(push_constant) uniform Push {
    float angle;
    float aspect;
} pc;

layout(location = 0) out vec3 vColor;

void main() {
    // Three corners 120 degrees apart, rotating on the pushed angle.
    float a = pc.angle + float(gl_VertexIndex) * 2.0943951;
    vec2 p = vec2(cos(a), sin(a)) * 0.6;
    p.x /= pc.aspect;

    gl_Position = vec4(p, 0.0, 1.0);
    vColor = vec3(gl_VertexIndex == 0 ? 1.0 : 0.0,
                  gl_VertexIndex == 1 ? 1.0 : 0.0,
                  gl_VertexIndex == 2 ? 1.0 : 0.0);
}
