#version 450

layout(location = 0) in vec2 screenUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ScenePush {
    float timeSeconds;
    float aspectRatio;
    float lightingMode;
} scene;

void considerHit(
    float t,
    vec3 ro,
    vec3 rd,
    vec3 surfaceNormal,
    int surfaceMaterial,
    inout float closest,
    inout vec3 hitPosition,
    inout vec3 hitNormal,
    inout int materialId
) {
    if (t > 0.015 && t < closest) {
        closest = t;
        hitPosition = ro + rd * t;
        hitNormal = surfaceNormal;
        materialId = surfaceMaterial;
    }
}

vec3 getBaseColor(int materialId, vec3 p) {
    if (materialId == 1) {
        vec2 tile = abs(fract(vec2(p.x, p.z) * 0.45) - 0.5);
        float seam = 1.0 - smoothstep(0.018, 0.045, min(tile.x, tile.y));
        vec3 floorTile = mix(vec3(0.31, 0.37, 0.43), vec3(0.12, 0.17, 0.22), seam);
        return floorTile;
    }
    if (materialId == 2) {
        float panel = abs(fract(p.z * 0.32) - 0.5);
        float seam = 1.0 - smoothstep(0.012, 0.035, panel);
        return mix(vec3(0.24, 0.30, 0.36), vec3(0.10, 0.15, 0.20), seam);
    }
    if (materialId == 3) return vec3(0.37, 0.43, 0.49);
    if (materialId == 4) return vec3(0.34, 0.43, 0.52);
    if (materialId == 5) return vec3(0.035, 0.48, 0.72);
    return vec3(0.70, 0.31, 0.13);
}

void main() {
    // Camera rays are tested against a small fixed set of planes: no ray-march loop.
    vec2 uv = screenUv * 2.0 - 1.0;
    uv.y = -uv.y;
    uv.x *= scene.aspectRatio;

    vec3 ro = vec3(0.0, 0.12, scene.timeSeconds * 0.55);
    vec3 rd = normalize(vec3(uv.x * 0.78, uv.y * 0.72, 1.0));

    float closest = 1.0e20;
    vec3 hitPosition = vec3(0.0);
    vec3 hitNormal = vec3(0.0, 1.0, 0.0);
    int materialId = 0;

    if (rd.y < -0.015) {
        considerHit((-1.55 - ro.y) / rd.y, ro, rd, vec3(0.0, 1.0, 0.0), 1,
                    closest, hitPosition, hitNormal, materialId);
    }
    if (rd.y > 0.015) {
        considerHit((2.55 - ro.y) / rd.y, ro, rd, vec3(0.0, -1.0, 0.0), 2,
                    closest, hitPosition, hitNormal, materialId);
    }
    if (rd.x > 0.015) {
        considerHit((3.35 - ro.x) / rd.x, ro, rd, vec3(-1.0, 0.0, 0.0), 2,
                    closest, hitPosition, hitNormal, materialId);
    }
    if (rd.x < -0.015) {
        considerHit((-3.35 - ro.x) / rd.x, ro, rd, vec3(1.0, 0.0, 0.0), 2,
                    closest, hitPosition, hitNormal, materialId);
    }

    // Three nearby cross-section rings give the tunnel depth without iterative marching.
    float firstRingZ = floor(ro.z / 3.0) * 3.0 + 3.0;
    for (int ring = 0; ring < 3; ++ring) {
        float ringZ = firstRingZ + float(ring) * 3.0;
        float ringT = (ringZ - ro.z) / rd.z;
        if (ringT > 0.015 && ringT < closest) {
            vec3 ringPoint = ro + rd * ringT;
            float ellipseRadius = length(vec2(ringPoint.x / 3.12,
                                              (ringPoint.y - 0.12) / 2.16));
            if (abs(ellipseRadius - 1.0) < 0.022) {
                vec3 ringNormal = normalize(vec3(
                    ringPoint.x / 9.73,
                    (ringPoint.y - 0.12) / 4.67,
                    0.0));
                considerHit(ringT, ro, rd, ringNormal, 4,
                            closest, hitPosition, hitNormal, materialId);
            }
        }
    }

    vec3 color = mix(vec3(0.012, 0.025, 0.045), vec3(0.035, 0.075, 0.11),
                     clamp(uv.y * 0.5 + 0.5, 0.0, 1.0));

    if (materialId != 0) {
        vec3 base = getBaseColor(materialId, hitPosition);

        // Repeating blue wall strips and warm ceiling lamps are emissive accents.
        float strip = 1.0 - smoothstep(0.0, 0.035, abs(abs(hitPosition.x) - 3.28));
        float lampBand = 1.0 - smoothstep(0.0, 0.035, abs(fract(hitPosition.z / 3.0) - 0.5));
        vec3 emission = vec3(0.02, 0.22, 0.38) * strip;
        if (materialId == 2) emission += vec3(0.30, 0.095, 0.025) * lampBand;

        if (scene.lightingMode < 0.5) {
            // OFF is an inexpensive unlit material pass, not a blank scene.
            color = base + emission;
        } else {
            vec3 keyLight = normalize(vec3(-0.45, 0.82, -0.34));
            float diffuse = max(dot(hitNormal, keyLight), 0.0);
            float fill = max(dot(hitNormal, normalize(vec3(0.55, 0.22, -0.4))), 0.0);
            float brightness = 0.40 + diffuse * 0.82 + fill * 0.20;
            color = base * brightness + emission;

            if (scene.lightingMode > 1.5) {
                // MAX adds only a small specular calculation; no shadow/AO sampling loops.
                vec3 viewDirection = normalize(ro - hitPosition);
                vec3 halfVector = normalize(keyLight + viewDirection);
                float specular = pow(max(dot(hitNormal, halfVector), 0.0), 18.0);
                color += vec3(0.55, 0.78, 1.0) * specular * 0.28;
                color *= 1.12;
            }
        }

        float fog = exp(-closest * 0.012);
        color = mix(vec3(0.012, 0.024, 0.042), color, fog);
    }

    color = color / (color + vec3(1.0));
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
