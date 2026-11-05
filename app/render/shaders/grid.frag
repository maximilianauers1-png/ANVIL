#version 450
layout(push_constant) uniform PC {
    mat4 invViewProj;  // inverse of projection * view rotation (camera-relative)
    vec4 eye;          // xyz: eye position (world, mm, relative to grid origin), w: minor step (mm)
    vec4 fwd;          // xyz: view forward, w: near plane (perspective)
    vec4 misc;         // x: ortho flag, y: ortho far, z: ortho near, w: opacity
    vec4 color;        // grid line color
} pc;
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

vec3 unproject(float z) {
    vec4 p = pc.invViewProj * vec4(vNdc, z, 1.0);
    return p.xyz / p.w;
}
float lines(vec2 xy, float step) {
    vec2 g = xy / step;
    vec2 w = fwidth(g);
    vec2 d = abs(fract(g - 0.5) - 0.5) / max(w, vec2(1e-6));
    return 1.0 - clamp(min(d.x, d.y), 0.0, 1.0);
}
void main() {
    vec3 o = unproject(1.0);           // near plane (reversed Z)
    vec3 d = unproject(0.5) - o;       // finite point further along the ray
    float t = (-pc.eye.z - o.z) / d.z; // plane z_world = 0
    if (!(t > 0.0)) discard;
    vec3 hit = o + t * d;              // camera-relative
    vec2 xy = hit.xy + pc.eye.xy;      // world
    float s = pc.eye.w;
    float minor = lines(xy, s), major = lines(xy, 10.0 * s);
    float dist = length(hit);
    float fade = 1.0 - smoothstep(200.0 * s, 1000.0 * s, dist);
    vec2 axw = fwidth(xy);
    float ax = 1.0 - clamp(abs(xy.y) / max(axw.y, 1e-6), 0.0, 1.0);  // X axis (y = 0)
    float ay = 1.0 - clamp(abs(xy.x) / max(axw.x, 1e-6), 0.0, 1.0);  // Y axis (x = 0)
    vec3 c = pc.color.rgb;
    float a = max(minor * 0.35, major * 0.8) * pc.misc.w * fade;
    if (ax > 0.0) { c = mix(c, vec3(0.86, 0.26, 0.24), ax); a = max(a, ax * 0.9 * fade); }
    if (ay > 0.0) { c = mix(c, vec3(0.30, 0.72, 0.30), ay); a = max(a, ay * 0.9 * fade); }
    if (a <= 0.003) discard;
    outColor = vec4(c, a);
    float viewZ = dot(hit, pc.fwd.xyz);
    gl_FragDepth = pc.misc.x > 0.5 ? (pc.misc.y - viewZ) / (pc.misc.y - pc.misc.z) : pc.fwd.w / viewZ;
}
