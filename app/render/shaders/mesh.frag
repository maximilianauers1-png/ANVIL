#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 vRel;
layout(location = 0) out vec4 outColor;
void main() {
    // Flat normal from screen-space derivatives: shows exactly the facets that
    // will be printed and needs no normal attribute (12 bytes per vertex).
    vec3 n = normalize(cross(dFdx(vRel), dFdy(vRel)));
    vec3 v = normalize(-vRel);
    if (dot(n, v) < 0.0) n = -n;
    float head = max(dot(n, v), 0.0);
    vec3 key = normalize(v + vec3(0.35, 0.25, 0.6));
    float k = max(dot(n, key), 0.0);
    float hemi = 0.5 + 0.5 * n.z;                 // world Z up
    vec3 base = pc.color.rgb;
    vec3 c = base * (0.18 + 0.50 * head + 0.17 * k + 0.15 * hemi);
    c += vec3(0.10) * pow(max(dot(reflect(-v, n), v), 0.0), 24.0);
    c = mix(c, vec3(1.0, 0.62, 0.18), 0.35 * pc.params.x);  // selection tint
    outColor = vec4(c, pc.color.a);
}
