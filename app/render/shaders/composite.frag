#version 450
layout(binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(texture(tex, vNdc * 0.5 + 0.5).rgb, 1.0); }
