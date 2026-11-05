// Push constants shared by mesh and edge pipelines (112 bytes <= 128 guaranteed).
layout(push_constant) uniform PC {
    mat4 viewProj;   // projection * view rotation (camera-relative, no translation)
    vec4 offset;     // object origin - eye (computed in double on the CPU)
    vec4 color;      // rgb, a = opacity
    vec4 params;     // x: highlight (0..1), y: relative depth bias (perspective), z: absolute depth bias (ortho)
} pc;
