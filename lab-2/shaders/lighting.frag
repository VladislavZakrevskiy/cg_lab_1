#version 450

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 0) out vec4 outColor;

layout(std140, set = 0, binding = 0) uniform Scene {
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 ambient;
    vec4 directionalDirection;
    vec4 directionalColorIntensity;
    ivec4 lightCounts;
} scene;

layout(std140, set = 1, binding = 0) uniform Model {
    mat4 model;
    mat4 normalMatrix;
    vec4 albedo;
    vec4 specularShininess;
} object;

struct PointLight {
    vec4 position;
    vec4 colorIntensity;
};

struct SpotLight {
    vec4 position;
    vec4 colorIntensity;
    vec4 directionCosInner;
    vec4 cone;
};

layout(std430, set = 0, binding = 1) readonly buffer PointLights {
    PointLight points[];
};

layout(std430, set = 0, binding = 2) readonly buffer SpotLights {
    SpotLight spots[];
};

vec3 safeNormalize(vec3 value) {
    return value / sqrt(max(dot(value, value), 1e-8));
}

vec3 blinnPhong(vec3 N, vec3 V, vec3 L, vec3 radiance) {
    float diffuse = max(dot(N, L), 0.0);
    if (diffuse <= 0.0)
        return vec3(0.0);

    vec3 H = safeNormalize(L + V);
    float specular = pow(max(dot(N, H), 0.0), object.specularShininess.w);
    return radiance * (object.albedo.rgb * diffuse + object.specularShininess.rgb * specular);
}

void main() {
    vec3 N = safeNormalize(worldNormal);
    vec3 V = safeNormalize(scene.cameraPosition.xyz - worldPosition);
    vec3 color = scene.ambient.rgb * scene.ambient.w * object.albedo.rgb;

    color += blinnPhong(N, V, -scene.directionalDirection.xyz,
                       scene.directionalColorIntensity.rgb * scene.directionalColorIntensity.w);

    for (int i = 0; i < scene.lightCounts.x; ++i) {
        vec3 delta = points[i].position.xyz - worldPosition;
        float distanceSquared = max(dot(delta, delta), 0.01);
        vec3 radiance = points[i].colorIntensity.rgb * points[i].colorIntensity.w / distanceSquared;
        color += blinnPhong(N, V, safeNormalize(delta), radiance);
    }

    for (int i = 0; i < scene.lightCounts.y; ++i) {
        vec3 delta = spots[i].position.xyz - worldPosition;
        float distanceSquared = max(dot(delta, delta), 0.01);
        vec3 L = safeNormalize(delta);
        float cosine = dot(-L, spots[i].directionCosInner.xyz);
        float inner = spots[i].directionCosInner.w;
        float outer = spots[i].cone.x;
        // Equal angles mean a hard edge; smoothstep requires distinct edges.
        float cone = inner > outer ? smoothstep(outer, inner, cosine) : step(outer, cosine);
        vec3 radiance = spots[i].colorIntensity.rgb * spots[i].colorIntensity.w * cone / distanceSquared;
        color += blinnPhong(N, V, L, radiance);
    }

    outColor = vec4(color, 1.0);
}
