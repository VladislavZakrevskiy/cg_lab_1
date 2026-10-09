#version 450

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 color;
layout(location = 0) out vec3 vertexColor;

layout(std140, set = 0, binding = 0) uniform Scene {
    mat4 model;
    mat4 view;
    mat4 projection;
    vec4 baseColor;
} scene;

void main() {
    gl_Position = scene.projection * scene.view * scene.model * vec4(position, 1.0);
    vertexColor = color * scene.baseColor.rgb;
}
