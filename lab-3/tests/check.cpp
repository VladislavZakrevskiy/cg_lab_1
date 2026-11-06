#ifdef NDEBUG
#undef NDEBUG
#endif
#include "../source/scene.hpp"
#include <cassert>
#include <iostream>

bool near(float a, float b) {
    return std::abs(a - b) < 1e-4f;
}

int main() {
    using namespace lab;
    const auto vertices = makeVertices();
    for (size_t i = 0; i < vertices.size(); i += 3) {
        const auto a = vertices[i], b = vertices[i + 1], c = vertices[i + 2];
        assert(near(glm::length(a.position), 1));
        assert(near(glm::length(a.normal), 1));
        assert(glm::dot(a.normal, a.position) > 0);
        assert(near(glm::dot(a.normal, b.position - a.position), 0));
        assert(near(glm::dot(a.normal, c.position - a.position), 0));
        assert(glm::length(a.normal - b.normal) == 0 && glm::length(a.normal - c.normal) == 0);
        assert(near(glm::distance(a.position, b.position), glm::distance(a.position, c.position)));
    }

    for (size_t alignment : {size_t(16), size_t(64), size_t(256)}) {
        const auto stride = modelUniformStride(sizeof(ModelUniform), alignment);
        assert(stride >= sizeof(ModelUniform) && stride % alignment == 0);
        assert(stride + sizeof(ModelUniform) <= 2 * stride);
    }
    for (const auto& vertex : vertices) {
        assert(vertex.uv.x >= 0 && vertex.uv.x <= 1);
        assert(vertex.uv.y >= 0 && vertex.uv.y <= 1);
    }
    assert(glm::length(vertices[0].uv - vertices[1].uv) > 0);
    Camera camera;
    const auto origin = viewMatrix(camera) * glm::vec4(0, 0, 0, 1);
    assert(near(origin.x, 0) && near(origin.y, 0) && near(origin.z, -5));
    for (float pitch : {-89.0f, 0.0f, 89.0f}) {
        camera.pitch = pitch;
        camera.roll = 45;
        const auto view = viewMatrix(camera);
        const auto eye = view * glm::vec4(camera.position, 1);
        const auto front = view * glm::vec4(camera.position + direction(camera.yaw, pitch), 1);
        assert(near(glm::length(glm::vec3(eye)), 0));
        assert(near(front.x, 0) && near(front.y, 0) && near(front.z, -1));
    }

    const auto projection = projectionMatrix(45, 16.0f / 9.0f);
    const auto nearPoint = projection * glm::vec4(0, 0, -0.1f, 1);
    const auto farPoint = projection * glm::vec4(0, 0, -100, 1);
    assert(near(nearPoint.z / nearPoint.w, 0));
    assert(near(farPoint.z / farPoint.w, 1));
    assert(projection[1][1] < 0);

    Model model;
    model.scale = {2, 0.5f, 3};
    model.position = {4, -2, 1};
    const auto matrix = modelMatrix(model);
    const auto normalMatrix = glm::transpose(glm::inverse(matrix));
    const auto normal = glm::vec3(normalMatrix * glm::vec4(vertices[0].normal, 0));
    const auto tangent = glm::vec3(matrix * glm::vec4(vertices[1].position - vertices[0].position, 0));
    assert(near(glm::dot(normal, tangent), 0));

    SpotLight spot;
    auto packed = spotUniform(spot);
    assert(near(glm::length(glm::vec3(packed.directionCosInner)), 1));
    assert(packed.directionCosInner.w > packed.cone.x);
    const float midpoint = (packed.directionCosInner.w + packed.cone.x) / 2;
    assert(near(glm::smoothstep(packed.cone.x, packed.directionCosInner.w, midpoint), 0.5f));
    spot.innerAngle = spot.outerAngle;
    packed = spotUniform(spot);
    assert(packed.directionCosInner.w == packed.cone.x);

    Scene scene;
    assert(scene.points.empty() && scene.spots.empty());
    for (int i = 0; i < 100; ++i) {
        scene.points.emplace_back();
        scene.spots.emplace_back();
    }
    assert(scene.points.size() == 100 && scene.spots.size() == 100);
    scene.points.erase(scene.points.begin() + 50);
    scene.spots.erase(scene.spots.begin());
    assert(scene.points.size() == 99 && scene.spots.size() == 99);
    scene = {};
    assert(scene.points.empty() && scene.spots.empty());
    std::cout << "PASS: geometry, camera, projection, normal matrix, spotlight packing, light arrays, GPU layouts\n";
}
