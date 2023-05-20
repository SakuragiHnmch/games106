#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;

layout (binding = 0) uniform UBO
{
	mat4 projection;
	mat4 view;
} ubo;

layout (location = 0) out vec3 outWorldPos;

out gl_PerVertex
{
	vec4 gl_Position;
};

void main()
{
	outWorldPos = inPos;

	// put the camera(eyes) in the center of world
	gl_Position = ubo.projection * ubo.view * vec4(inPos.xyz, 1.0);
}
