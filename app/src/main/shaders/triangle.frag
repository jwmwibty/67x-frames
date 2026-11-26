#version 450

layout(location = 0) in vec2 screenUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform ScenePush {
    float timeSeconds;
    float aspectRatio;
} scene;

float sdBox(vec3 p, vec3 b) {
    vec3 q = abs(p) - b;
    return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}

// A repeating industrial tunnel built entirely from signed-distance geometry.
float sceneMap(vec3 p, out int materialId) {
    float d = p.y + 1.55;
    materialId = 1;

    float ceiling = 2.45 - p.y;
    if (ceiling < d) { d = ceiling; materialId = 2; }

    float walls = 3.40 - abs(p.x);
    if (walls < d) { d = walls; materialId = 2; }

    vec3 q = p;
    q.z = mod(p.z + 1.5, 3.0) - 1.5;

    float pillars = min(
        sdBox(q - vec3(-2.78, -0.10, 0.0), vec3(0.16, 1.35, 0.18)),
        sdBox(q - vec3( 2.78, -0.10, 0.0), vec3(0.16, 1.35, 0.18)));
    if (pillars < d) { d = pillars; materialId = 3; }

    vec2 archPoint = vec2(p.x / 3.12, (p.y - 0.12) / 2.16);
    float archCurve = (length(archPoint) - 1.0) * 0.16 - 0.035;
    float arch = max(archCurve, abs(q.z) - 0.075);
    if (arch < d) { d = arch; materialId = 4; }

    float strips = min(
        sdBox(q - vec3(-2.96, 0.10, 0.0), vec3(0.035, 1.15, 0.025)),
        sdBox(q - vec3( 2.96, 0.10, 0.0), vec3(0.035, 1.15, 0.025)));
    if (strips < d) { d = strips; materialId = 5; }

    float ceilingLamp = sdBox(q - vec3(0.0, 2.22, 0.0), vec3(0.52, 0.035, 0.12));
    if (ceilingLamp < d) { d = ceilingLamp; materialId = 6; }

    return d;
}

vec3 sceneNormal(vec3 p) {
    const float e = 0.0025;
    int unusedMaterial;
    vec3 k = vec3(1.0, -1.0, 0.0);
    return normalize(
        k.xyy * sceneMap(p + k.xyy * e, unusedMaterial) +
        k.yyx * sceneMap(p + k.yyx * e, unusedMaterial) +
        k.yxy * sceneMap(p + k.yxy * e, unusedMaterial) +
        k.xxx * sceneMap(p + k.xxx * e, unusedMaterial));
}

float softShadow(vec3 ro, vec3 rd) {
    float result = 1.0;
    float t = 0.045;
    int unusedMaterial;
    for (int i = 0; i < 12; ++i) {
        float h = sceneMap(ro + rd * t, unusedMaterial);
        result = min(result, 9.0 * h / t);
        t += clamp(h, 0.035, 0.45);
        if (result < 0.025 || t > 8.0) break;
    }
    return clamp(result, 0.0, 1.0);
}

float ambientOcclusion(vec3 p, vec3 n) {
    float result = 0.0;
    float weight = 1.0;
    int unusedMaterial;
    for (int i = 0; i < 5; ++i) {
        float h = 0.035 + 0.13 * float(i);
        float d = sceneMap(p + n * h, unusedMaterial);
        result += (h - d) * weight;
        weight *= 0.62;
    }
    return clamp(1.0 - result * 1.8, 0.28, 1.0);
}

vec3 materialColor(int materialId, vec3 p) {
    if (materialId == 1) {
        vec2 cell = abs(fract(vec2(p.x * 1.2, p.z * 0.5)) - 0.5);
        float seam = 1.0 - smoothstep(0.018, 0.055, min(cell.x, cell.y));
        return mix(vec3(0.15, 0.19, 0.24), vec3(0.065, 0.085, 0.11), seam * 0.72);
    }
    if (materialId == 2) return vec3(0.105, 0.14, 0.19);
    if (materialId == 3) return vec3(0.28, 0.32, 0.38);
    if (materialId == 4) return vec3(0.16, 0.22, 0.29);
    if (materialId == 5) return vec3(0.08, 0.72, 1.0);
    return vec3(0.95, 0.43, 0.12);
}

void main() {
    vec2 uv = screenUv * 2.0 - 1.0;
    uv.y = -uv.y;
    uv.x *= scene.aspectRatio;

    vec3 ro = vec3(0.0, 0.05, scene.timeSeconds * 1.25);
    vec3 rd = normalize(vec3(uv.x * 0.76, uv.y * 0.62, 1.0));

    float travel = 0.025;
    int materialId = 0;
    bool hit = false;

    // Deliberately substantial per-pixel workload for the mobile GPU benchmark.
    for (int stepIndex = 0; stepIndex < 64; ++stepIndex) {
        vec3 p = ro + rd * travel;
        float distanceToScene = sceneMap(p, materialId);
        if (distanceToScene < 0.0035) { hit = true; break; }
        travel += max(distanceToScene * 0.78, 0.012);
        if (travel > 42.0) break;
    }

    vec3 color = vec3(0.012, 0.020, 0.038);
    if (hit) {
        vec3 p = ro + rd * travel;
        vec3 n = sceneNormal(p);
        vec3 lightPosition = vec3(0.0, 1.85, ro.z + 3.2);
        vec3 toLight = lightPosition - p;
        float lightDistance = length(toLight);
        vec3 lightDirection = toLight / max(lightDistance, 0.001);
        float diffuse = max(dot(n, lightDirection), 0.0);
        float shadow = softShadow(p + n * 0.012, lightDirection);
        float ao = ambientOcclusion(p, n);
        vec3 viewDirection = normalize(ro - p);
        vec3 halfDirection = normalize(lightDirection + viewDirection);
        float specular = pow(max(dot(n, halfDirection), 0.0), 40.0);

        vec3 base = materialColor(materialId, p);
        float attenuation = 1.0 / (1.0 + 0.055 * lightDistance * lightDistance);
        color = base * (0.12 + diffuse * shadow * attenuation * 1.55) * ao;
        color += vec3(0.55, 0.72, 1.0) * specular * shadow * 0.34;
        if (materialId == 5) color += vec3(0.015, 0.52, 0.95) * 1.25;
        if (materialId == 6) color += vec3(1.0, 0.30, 0.075) * 1.8;

        float fog = exp(-travel * 0.035);
        color = mix(vec3(0.012, 0.022, 0.045), color, fog);
    }

    color = color / (color + vec3(1.0));
    color = pow(max(color, vec3(0.0)), vec3(1.0 / 2.2));
    outColor = vec4(color, 1.0);
}
