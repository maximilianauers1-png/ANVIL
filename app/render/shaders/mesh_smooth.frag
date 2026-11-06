#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 vRel;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 n = normalize(vNormal);
    vec3 v = normalize(-vRel);
    if (dot(n, v) < 0.0) n = -n;
    // same light rig as mesh.frag, plus a slightly stronger key light and a
    // rim term so that sculpted relief reads at a glance
    float head = max(dot(n, v), 0.0);
    vec3 key = normalize(v + vec3(0.35, 0.25, 0.6));
    float k = max(dot(n, key), 0.0);
    float hemi = 0.5 + 0.5 * n.z;
    float rim = pow(1.0 - head, 3.0);
    vec3 base = pc.color.rgb;
    vec3 c = base * (0.14 + 0.46 * head + 0.28 * k + 0.12 * hemi) + vec3(0.08) * rim;
    c += vec3(0.12) * pow(max(dot(reflect(-v, n), key), 0.0), 32.0);
    c = mix(c, vec3(1.0, 0.62, 0.18), 0.35 * pc.params.x);
    outColor = vec4(c, pc.color.a);
}
