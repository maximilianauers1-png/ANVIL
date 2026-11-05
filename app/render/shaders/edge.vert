#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec3 inPos;
void main() {
    gl_Position = pc.viewProj * vec4(inPos + pc.offset.xyz, 1.0);
    // Pull lines ~2 px towards the camera (reversed Z: larger = closer).
    // Perspective: depth = n/z, so a relative factor moves by a fixed angle;
    // orthographic: depth is linear, so an absolute offset is used.
    gl_Position.z = gl_Position.z * (1.0 + pc.params.y) + pc.params.z * gl_Position.w;
}
