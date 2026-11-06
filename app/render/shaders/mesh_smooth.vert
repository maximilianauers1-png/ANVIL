#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
// Sculpted surfaces: per-vertex normals (trilinear SDF gradient) for smooth
// shading; hard-surface parts keep the flat facet shading of mesh.vert.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 0) out vec3 vRel;
layout(location = 1) out vec3 vNormal;
void main() {
    vec3 p = inPos + pc.offset.xyz;
    vRel = p;
    vNormal = inNormal;
    gl_Position = pc.viewProj * vec4(p, 1.0);
}
