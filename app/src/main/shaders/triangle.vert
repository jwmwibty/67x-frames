#version 450

layout(location = 0) out vec3 vertexColor;

const vec2 positions[3] = vec2[](
    vec2( 0.0, -0.68),
    vec2( 0.68, 0.58),
    vec2(-0.68, 0.58)
);

const vec3 colors[3] = vec3[](
    vec3(1.00, 0.25, 0.22),
    vec3(0.20, 0.85, 0.48),
    vec3(0.20, 0.55, 1.00)
);

void main() {
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vertexColor = colors[gl_VertexIndex];
}
