#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace lab {

struct Vertex {
    glm::vec3 position, normal;
};

inline std::array<Vertex, 60> makeVertices() {
    const float phi = (1.0f + std::sqrt(5.0f)) / 2.0f;
    std::array<glm::vec3, 12> points = {{
        {-1, phi, 0}, {1, phi, 0}, {-1, -phi, 0}, {1, -phi, 0},
        {0, -1, phi}, {0, 1, phi}, {0, -1, -phi}, {0, 1, -phi},
        {phi, 0, -1}, {phi, 0, 1}, {-phi, 0, -1}, {-phi, 0, 1},
    }};
    for (auto& p : points)
        p = glm::normalize(p);

    constexpr std::array<unsigned, 60> indices = {{
        0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11,
        1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
        3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9,
        4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1,
    }};

    std::array<Vertex, 60> vertices{};
    for (size_t i = 0; i < indices.size(); i += 3) {
        const auto a = points[indices[i]], b = points[indices[i + 1]], c = points[indices[i + 2]];
        const auto normal = glm::normalize(glm::cross(b - a, c - a));
        vertices[i] = {a, normal};
        vertices[i + 1] = {b, normal};
        vertices[i + 2] = {c, normal};
    }
    return vertices;
}

inline glm::vec3 direction(float yaw, float pitch) {
    const float y = glm::radians(yaw), p = glm::radians(pitch);
    return {std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y)};
}

struct Camera {
    glm::vec3 position{0, 0, 5};
    float yaw = -90, pitch = 0, roll = 0, fov = 45;
};

inline glm::mat4 viewMatrix(const Camera& camera) {
    const auto forward = direction(camera.yaw, camera.pitch);
    const auto right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
    auto up = glm::cross(right, forward);
    up = glm::vec3(glm::rotate(glm::mat4(1), glm::radians(camera.roll), forward) * glm::vec4(up, 0));
    return glm::lookAt(camera.position, camera.position + forward, up);
}

inline glm::mat4 projectionMatrix(float fov, float aspect) {
    auto projection = glm::perspective(glm::radians(fov), aspect, 0.1f, 100.0f);
    projection[1][1] *= -1.0f;
    return projection;
}

struct Model {
    glm::vec3 position{0}, rotation{15, 25, 0}, scale{1};
    glm::vec3 albedo{0.65f, 0.42f, 0.2f}, specular{1};
    float shininess = 32;
};

inline glm::mat4 modelMatrix(const Model& model) {
    auto matrix = glm::translate(glm::mat4(1), model.position);
    matrix = glm::rotate(matrix, glm::radians(model.rotation.z), glm::vec3(0, 0, 1));
    matrix = glm::rotate(matrix, glm::radians(model.rotation.y), glm::vec3(0, 1, 0));
    matrix = glm::rotate(matrix, glm::radians(model.rotation.x), glm::vec3(1, 0, 0));
    return glm::scale(matrix, model.scale);
}

struct alignas(16) PointLight {
    glm::vec4 position{2, 2, 2, 1};
    glm::vec4 colorIntensity{1, 0.65f, 0.35f, 8};
};

struct SpotLight {
    PointLight light{{0, 0, 3, 1}, {0.4f, 0.65f, 1, 12}};
    float yaw = -90, pitch = 0, innerAngle = 12, outerAngle = 22;
};

struct alignas(16) SpotUniform {
    glm::vec4 position, colorIntensity, directionCosInner, cone;
};

inline SpotUniform spotUniform(const SpotLight& spot) {
    return {spot.light.position, spot.light.colorIntensity,
            glm::vec4(direction(spot.yaw, spot.pitch), std::cos(glm::radians(spot.innerAngle))),
            glm::vec4(std::cos(glm::radians(spot.outerAngle)), 0, 0, 0)};
}

struct Scene {
    Camera camera;
    Model model;
    glm::vec3 ambientColor{1}, directionalColor{1};
    float ambientIntensity = 0.08f, directionalIntensity = 0.8f;
    float lightYaw = -120, lightPitch = -45;
    std::vector<PointLight> points;
    std::vector<SpotLight> spots;
};

struct alignas(16) SceneUniform {
    glm::mat4 viewProjection;
    glm::vec4 cameraPosition, ambient, directionalDirection, directionalColorIntensity;
    glm::ivec4 lightCounts;
};

struct alignas(16) ModelUniform {
    glm::mat4 model, normalMatrix;
    glm::vec4 albedo, specularShininess;
};

static_assert(sizeof(Vertex) == 24 && offsetof(Vertex, normal) == 12);
static_assert(sizeof(PointLight) == 32 && offsetof(PointLight, colorIntensity) == 16);
static_assert(sizeof(SpotUniform) == 64 && offsetof(SpotUniform, colorIntensity) == 16);
static_assert(offsetof(SpotUniform, directionCosInner) == 32 && offsetof(SpotUniform, cone) == 48);
static_assert(offsetof(SceneUniform, cameraPosition) == 64 && offsetof(SceneUniform, ambient) == 80);
static_assert(offsetof(SceneUniform, directionalDirection) == 96);
static_assert(offsetof(SceneUniform, directionalColorIntensity) == 112);
static_assert(offsetof(SceneUniform, lightCounts) == 128 && sizeof(SceneUniform) == 144);
static_assert(offsetof(ModelUniform, normalMatrix) == 64 && offsetof(ModelUniform, albedo) == 128);
static_assert(offsetof(ModelUniform, specularShininess) == 144 && sizeof(ModelUniform) == 160);

} // namespace lab
