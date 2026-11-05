#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 inPos;
layout(location = 0) out vec3 vRel;
void main() {
    vec3 p = inPos + pc.offset.xyz;  // camera-relative position (eye at origin)
    vRel = p;
    gl_Position = pc.viewProj * vec4(p, 1.0);
}
